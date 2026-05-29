#ifndef CLOUD_CONFIG_H
#define CLOUD_CONFIG_H

/* Supabase project credentials for the QueSort online-fallback path.
 *
 * The anon (public) key is meant to be embedded in client code — it
 * scopes to the RLS policies we set on knob_states (SELECT/INSERT/
 * UPDATE for anon, DELETE for service_role only). Knobs self-identify
 * by (store_id, mac); no user auth.
 *
 * If a key rotation happens, the dashboard's Lovable redeploy will
 * print new values — paste them here, rebuild, OTA push.
 */

#define CLOUD_SUPABASE_URL      "https://gsluyvaznlgduqwoiaar.supabase.co"
#define CLOUD_SUPABASE_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImdzbHV5dmF6bmxnZHVxd29pYWFyIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODAwMTQ2MDgsImV4cCI6MjA5NTU5MDYwOH0.fKLNIW2m342IoYFbZ8N2ancMYfFetwlkpyCwWk-ux-E"

/* PostgREST endpoint path. UPSERT semantics via Prefer header. */
#define CLOUD_TABLE_PATH        "/rest/v1/knob_states"

/* How often to POST a heartbeat when nothing else has changed.
 * Updates `last_seen` so dashboards can flag stale knobs. */
#define CLOUD_HEARTBEAT_MS      10000U

#endif /* CLOUD_CONFIG_H */
