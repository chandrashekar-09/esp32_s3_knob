# ESP-WIFI-MESH Test Project

## Project Overview
This project demonstrates the implementation of an ESP-WIFI-MESH network using ESP32 devices. The ESP-MESH protocol enables multiple ESP32 devices to form a self-healing, scalable, and robust mesh network. The project is modularized for reusability and ease of integration into other projects.

## Features
- **ESP-WIFI-MESH Protocol**: Implements a mesh network where nodes can communicate directly or via intermediate nodes.
- **Modular Design**: Code is structured into reusable components for better maintainability.
- **PlatformIO Integration**: Simplifies building, uploading, and managing the project.
- **LED Indicators**: Visual feedback for mesh status.

## Project Structure
```
CMakeLists.txt
platformio.ini
sdkconfig.esp32dev
include/
	README
lib/
	mesh_light.h
src/
	CMakeLists.txt
	mesh_light.c
	mesh_main.c
test/
	README
```

### Key Files
- **`src/mesh_main.c`**: Entry point for the application. Initializes and starts the mesh network.
- **`include/mesh_manager.h`**: API definitions for the mesh manager module.
- **`src/mesh_manager.cpp`**: Implements the mesh manager, including initialization, event handling, and P2P communication.
- **`include/mesh_light.h`**: Controls LED indicators for mesh status.
- **`platformio.ini`**: Configuration file for PlatformIO.

## Technical Details
### ESP-WIFI-MESH
ESP-WIFI-MESH is a networking protocol developed by Espressif Systems for ESP32 devices. It allows devices to form a mesh network where nodes can communicate with each other directly or through intermediate nodes. This protocol is ideal for IoT applications requiring scalability and reliability.

#### Key Features
- **Self-Healing**: Automatically reconfigures the network when nodes join or leave.
- **Scalability**: Supports a large number of nodes.
- **Robustness**: Ensures reliable communication even in dynamic environments.

#### Mesh Formation
1. **Root Node**: Connects to the router and acts as the gateway for the mesh network.
2. **Intermediate Nodes**: Relay messages between nodes.
3. **Leaf Nodes**: End devices that communicate through the mesh.

### Implementation Details
- **Initialization**: The root node connects to the router before forming the mesh.
- **Event Handling**: The `mesh_manager` module handles events such as node joining, leaving, and data transmission.
- **P2P Communication**: Nodes can send and receive messages directly or via intermediate nodes.

## Build and Upload Instructions
1. **Clean the Project**:
   ```bash
   platformio run --target clean
   ```
2. **Build the Project**:
   ```bash
   platformio run
   ```
3. **Upload the Firmware**:
   ```bash
   platformio run --target upload
   ```
4. **Erase Flash (Optional)**:
   ```bash
   platformio run --target erase
   ```

## Logs and Debugging
- Use the serial monitor to view logs and debug messages.
- Verify that the root node connects to the router before the mesh forms.

## Future Enhancements
- Add a status API to query mesh/router connection states programmatically.
- Implement over-the-air (OTA) updates for firmware.

## References
- [ESP-MESH Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/mesh.html)
- [PlatformIO Documentation](https://docs.platformio.org/)

---

This project is developed and tested using ESP32 devices and the ESP-IDF framework. Contributions and feedback are welcome!