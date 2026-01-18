# obs-preview-streamer
A lightweight OBS Studio filter plugin that captures the Preview screen in real-time and streams NV21-formatted frames over TCP, especially suitable for pushing the OBS Preview to a mobile device for remote monitoring.

一个轻量级的 OBS Studio 滤镜插件，用于实时捕获预览（Preview）画面并通过 TCP 推送 NV21 格式帧，特别适合将 OBS 预览画面实时推送到手机进行远程监看。

## Motivation
The goal of this project is to perform real-time visual processing (e.g., face swapping) on the PC side, while keeping the mobile device responsible only for receiving and consuming video frames. Traditional RTMP streaming typically introduces at least one second of latency, which is unacceptable for real-time interaction. NDI, while powerful, is unnecessarily heavyweight for this specific use case.

This plugin therefore adopts a minimal design: it is built purely on the OBS API and standard sockets, and streams frames directly over local TCP in Android’s native NV21 format. As a result, the end-to-end latency is close to zero.

### Requirements
- OBS Studio 28+ 
- CMake and a suitable compiler toolchain
	- macOS: Xcode Command Line Tools
	- Windows: Visual Studio
	- Linux: gcc

### Build Instructions
1. Build Instructions
   ```bash
   git clone https://github.com/BojackMa/obs-preview-streamer.git
   cd obs-preview-streamer
   ```

2. Create the build directory and compile
   ```bash
   mkdir build && cd build
   cmake .. -DOBS_INCLUDE_DIR=/path/to/obs-studio/include -DLIBOBS_LIB=/path/to/libobs
   make -j
   ```   

### Usage
1.	Enable Studio Mode in OBS.
2.	Add a filter to the Preview scene (or the top-level source/group you want to capture):
	- Right-click the source → Filters → +
	- Select Preview Streamer or Frame Logger (MT)
3.	(Optional) Adjust filter settings:
	- Log Interval (ms)
	- Smaller values provide better real-time performance at the cost of higher bandwidth usage


#### Mobile Receiver
On the mobile side, an Android application can connect to 127.0.0.1:27183 to receive preview frames from OBS. Depending on the deployment scenario, local port forwarding can be achieved via ADB forward or USB network sharing. The communication protocol is intentionally minimal: each frame starts with a 4-byte little-endian length header, followed by the complete NV21 frame payload, allowing the data to be parsed efficiently and passed directly into decoding or downstream processing on the mobile device.


### Implementation Overview
The plugin is implemented using the standard OBS filter rendering callback mechanism. During rendering, gs_texrender and gs_stagesurface are used to synchronously copy the target source’s GPU texture into CPU memory, following the same approach as official OBS examples to ensure compatibility and stability. Once RGBA data is obtained, it is converted into NV21, the most commonly used native format on Android. The conversion is performed pixel by pixel using BT.601 coefficients to avoid implicit color space assumptions and ensure consistent color accuracy.

To prevent blocking the OBS main rendering pipeline, the plugin employs a clear multi-threaded design. The render thread is responsible only for capturing frames and enqueuing them as quickly as possible, while format conversion and network transmission are handled by a background worker thread. Thread coordination is implemented using pthread primitives and dispatch semaphores. The networking layer uses a custom minimal TCP protocol with support for automatic reconnection, TCP_NODELAY, a large send buffer (4 MB), and explicit suppression of SIGPIPE to improve robustness. Internally, frames are managed using a linked-list queue protected by mutexes, with enqueue counters and logging to facilitate debugging, performance analysis, and dropped-frame diagnostics.

