/* Provisioning mode — SoftAP + captive portal.
 *
 * Started by phase_manager when the user long-presses the screen
 * for 10 s. Brings up an OPEN SoftAP and serves a small HTML
 * page from 192.168.4.1 with:
 *   - Online Mode toggle (on/off → knob_config_set_online_mode)
 *   - WiFi scan list (populated from /scan endpoint)
 *   - Password field
 *   - [Save & Reboot] button
 *
 * On Save, the page POSTs to /save which:
 *   1. knob_config_set_wifi(ssid, pass)
 *   2. knob_config_set_online_mode(toggle)
 *   3. knob_config_commit() → NVS
 *   4. Returns success page
 *   5. Schedules esp_restart() after a 1 s delay (so the response
 *      flushes to the browser before TCP dies).
 *
 * The HTML is embedded as a single C string. Plain HTML+JS, no
 * external assets; works without internet on the phone.
 */

#include "provisioning.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "knob_config.h"
#include "phase_manager.h"

static const char *kTag = "prov";

static bool             s_active            = false;
static httpd_handle_t   s_httpd             = NULL;
static esp_netif_t     *s_ap_netif          = NULL;
static char             s_softap_ssid[33]   = {0};
static char             s_qr_string[64]     = {0};

static void prov_delayed_restart_task(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(kTag, "rebooting to apply new config");
    esp_restart();
}

/* ────────────────────────── HTML page ──────────────────────────
 * Single-page UI. The Online Mode toggle sits ABOVE the network
 * list. Vanilla HTML + JS so it works offline on any phone.
 */
static const char *kIndexHtml =
"<!doctype html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>QueSort Setup</title>"
"<style>"
"  body{font-family:-apple-system,system-ui,sans-serif;background:#0a0a0a;color:#eee;margin:0;padding:20px}"
"  h1{margin:0 0 8px;font-size:22px;letter-spacing:1px}"
"  .sub{color:#888;font-size:13px;margin-bottom:24px}"
"  .toggle{display:flex;align-items:center;justify-content:space-between;background:#181818;border:1px solid #2a2a2a;border-radius:12px;padding:18px 20px;margin-bottom:24px}"
"  .toggle-label{font-size:17px;font-weight:600}"
"  .toggle-desc{color:#888;font-size:12px;margin-top:4px}"
"  .switch{position:relative;display:inline-block;width:56px;height:30px;flex-shrink:0;margin-left:16px}"
"  .switch input{opacity:0;width:0;height:0}"
"  .slider{position:absolute;cursor:pointer;inset:0;background:#444;border-radius:30px;transition:.2s}"
"  .slider:before{position:absolute;content:'';height:24px;width:24px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.2s}"
"  input:checked + .slider{background:#22c55e}"
"  input:checked + .slider:before{transform:translateX(26px)}"
"  h2{font-size:14px;text-transform:uppercase;letter-spacing:1px;color:#888;margin:0 0 12px}"
"  .net{display:flex;justify-content:space-between;align-items:center;background:#181818;padding:14px 16px;border-radius:10px;margin-bottom:8px;cursor:pointer;border:1px solid #2a2a2a}"
"  .net.sel{border-color:#22c55e;background:#181f18}"
"  .net-ssid{font-size:15px}"
"  .net-rssi{color:#888;font-size:12px}"
"  input[type=password]{width:100%;background:#181818;border:1px solid #2a2a2a;color:#eee;padding:14px;border-radius:10px;font-size:15px;box-sizing:border-box;margin-top:16px}"
"  button{width:100%;background:#22c55e;color:#000;border:0;padding:16px;border-radius:10px;font-size:16px;font-weight:600;margin-top:16px;cursor:pointer}"
"  button:disabled{background:#333;color:#666}"
"  .ok{background:#181818;padding:20px;border-radius:10px;text-align:center;color:#22c55e}"
"</style>"
"</head><body>"
"<h1>QueSort Setup</h1>"
"<div class='sub' id='knob'></div>"

"<div class='toggle'>"
"  <div>"
"    <div class='toggle-label'>Online Mode</div>"
"    <div class='toggle-desc'>OFF = mesh only (offline). ON = mesh + cloud fallback for cross-floor.</div>"
"  </div>"
"  <label class='switch'><input type='checkbox' id='online'><span class='slider'></span></label>"
"</div>"

"<h2>WiFi Network</h2>"
"<div id='nets'>Scanning...</div>"
"<input type='password' id='pass' placeholder='Network password' autocomplete='off'>"
"<button id='save' disabled>Save & Reboot</button>"

"<script>"
"let sel=null,nets=[];"
"function pickNet(s){sel=s;document.querySelectorAll('.net').forEach(e=>e.classList.remove('sel'));event.currentTarget.classList.add('sel');document.getElementById('save').disabled=false;}"
"async function scan(){"
"  try{const r=await fetch('/scan');nets=await r.json();"
"  const c=document.getElementById('nets');c.innerHTML='';"
"  if(nets.length===0){c.innerHTML='<div class=net>No networks found.</div>';return;}"
"  nets.forEach(n=>{const d=document.createElement('div');d.className='net';d.onclick=function(){pickNet(n.ssid);};"
"  d.innerHTML='<span class=net-ssid>'+n.ssid+'</span><span class=net-rssi>'+n.rssi+' dBm</span>';c.appendChild(d);});"
"  }catch(e){document.getElementById('nets').innerHTML='Scan failed: '+e;}"
"}"
"async function init(){"
"  try{const r=await fetch('/info');const j=await r.json();"
"  document.getElementById('knob').textContent='Knob '+j.id+' • MAC '+j.mac;"
"  document.getElementById('online').checked=j.online_mode;}catch(e){}"
"  scan();"
"}"
"document.getElementById('save').onclick=async()=>{"
"  const body={ssid:sel,pass:document.getElementById('pass').value,online_mode:document.getElementById('online').checked};"
"  const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"  document.body.innerHTML='<h1>QueSort Setup</h1><div class=ok>Saved! Knob is rebooting...</div>';"
"};"
"init();"
"</script>"
"</body></html>";

/* ────────────────────────── /info ─────────────────────────────
 * Returns the knob's identity + current online_mode so the page
 * can render the correct toggle position. */
static esp_err_t info_handler(httpd_req_t *req)
{
    const knob_config_t *cfg = knob_config_get();
    char body[160];
    snprintf(body, sizeof(body),
        "{\"id\":\"%s\",\"mac\":\"%s\",\"online_mode\":%s}",
        s_softap_ssid,
        s_softap_ssid + 9,   /* roughly: skip "QueSort-" */
        cfg->online_mode ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* ────────────────────────── /scan ─────────────────────────────
 * Returns JSON array of nearby APs. */
static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100, .scan_time.active.max = 300,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "scan_start failed: %s", esp_err_to_name(err));
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 16) n = 16;
    wifi_ap_record_t recs[16];
    esp_wifi_scan_get_ap_records(&n, recs);

    /* Build JSON inline. Up to 16 APs × ~80 bytes = 1.3 KB max. */
    char body[2048];
    int off = snprintf(body, sizeof(body), "[");
    for (int i = 0; i < n; i++) {
        const char *comma = (i + 1 < n) ? "," : "";
        off += snprintf(body + off, sizeof(body) - off,
            "{\"ssid\":\"%s\",\"rssi\":%d}%s",
            (const char *)recs[i].ssid, (int)recs[i].rssi, comma);
        if ((size_t)off >= sizeof(body) - 32) break;
    }
    snprintf(body + off, sizeof(body) - off, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* ────────────────────────── /save ─────────────────────────────
 * POST {ssid, pass, online_mode}. Writes to NVS and reboots. */
static esp_err_t save_handler(httpd_req_t *req)
{
    char body[512];
    int recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[recv] = '\0';

    /* Tiny JSON parsing — we control the producer, formats are
     * predictable. Look for "ssid":"X","pass":"Y","online_mode":true|false. */
    char ssid[KNOB_CFG_SSID_MAX] = {0};
    char pass[KNOB_CFG_PASS_MAX] = {0};
    bool online = true;

    const char *p;
    if ((p = strstr(body, "\"ssid\":\"")) != NULL) {
        p += 8;
        const char *e = strchr(p, '"');
        if (e && (size_t)(e - p) < sizeof(ssid)) {
            memcpy(ssid, p, e - p);
        }
    }
    if ((p = strstr(body, "\"pass\":\"")) != NULL) {
        p += 8;
        const char *e = strchr(p, '"');
        if (e && (size_t)(e - p) < sizeof(pass)) {
            memcpy(pass, p, e - p);
        }
    }
    if ((p = strstr(body, "\"online_mode\":")) != NULL) {
        online = (strncmp(p + 14, "true", 4) == 0);
    }

    ESP_LOGI(kTag, "/save: ssid='%s' online_mode=%s",
             ssid, online ? "ON" : "OFF");

    knob_config_set_wifi(ssid, pass);
    knob_config_set_online_mode(online);
    knob_config_commit();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    /* Defer reboot so the HTTP response actually reaches the phone. */
    xTaskCreate(prov_delayed_restart_task, "prov_reboot",
                2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, kIndexHtml);
}

/* Captive-portal hint: redirect any unknown path to the index. */
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* ────────────────────────── HTTP server ───────────────────────── */
static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t u_root = {.uri="/", .method=HTTP_GET, .handler=index_handler};
    httpd_uri_t u_info = {.uri="/info", .method=HTTP_GET, .handler=info_handler};
    httpd_uri_t u_scan = {.uri="/scan", .method=HTTP_GET, .handler=scan_handler};
    httpd_uri_t u_save = {.uri="/save", .method=HTTP_POST, .handler=save_handler};
    httpd_uri_t u_catch = {.uri="/*", .method=HTTP_GET, .handler=captive_redirect_handler};
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_info);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_catch);
    return ESP_OK;
}

/* ────────────────────────── public ─────────────────────────────── */
esp_err_t provisioning_start(void)
{
    if (s_active) return ESP_OK;

    /* Compute SoftAP SSID from MAC + identity. */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    app_state_t st;
    phase_manager_get_state(&st);
    snprintf(s_softap_ssid, sizeof(s_softap_ssid),
             "QueSort-%s%u-%02X%02X",
             st.device_type == DEV_TYPE_FR ? "FR" : "T",
             (unsigned)st.device_number,
             mac[4], mac[5]);
    snprintf(s_qr_string, sizeof(s_qr_string),
             "WIFI:S:%s;T:nopass;P:;;", s_softap_ssid);

    /* Need APSTA so scan_handler can still scan while AP is up. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* SoftAP config — open WiFi for one-tap join from QR. */
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_softap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len    = strlen(s_softap_ssid);
    ap.ap.channel     = 6;
    ap.ap.authmode    = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    esp_err_t err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_active = true;
    ESP_LOGI(kTag, "provisioning UP: SSID='%s' http://192.168.4.1/",
             s_softap_ssid);
    return ESP_OK;
}

void provisioning_stop(void)
{
    if (!s_active) return;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_active = false;
}

bool        provisioning_is_active(void)   { return s_active; }
const char *provisioning_softap_ssid(void) { return s_softap_ssid; }
const char *provisioning_qr_string(void)   { return s_qr_string; }
