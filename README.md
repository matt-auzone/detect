# Detection Model Service in C++

This is the Detection Model Service example, C++ edition, for the [DeepView AI Middleware][edgeai].  The example demonstrates how to use the VisionPack libraries VSL and VAAL to perform object detection on live video data.  The results are published over [ZeroMQ][zeromq] and can be used to stream to a browser using the [WebVision][webvision] service.

# Quick Start

Pre-built binaries are available under releases.  Your target should be running one of the images provided by [Yocto SDK for VisionPack][yocto-sdk] or a custom image with the requirements below.

Copy the `detect` binary along with `camera.sh` or `video.sh` scripts to the target.

Run `camera.sh` or `video.sh` scripts.  Note that `video.sh` will play to the end of the video file, then terminate.

Run `detect -v MODEL` with your DeepViewRT model, the `-v` option will cause verbose logging of the JSON output to the console.

Optionally, download and install [WebVision][webvision] for a remote browser display of the video and results.

# Requirements

- VideoStream 1.1.15+
- VAAL 1.2.16+
- ZeroMQ 4.3.4+

# Compile

The application can be built with either CMake or using the Makefile.  There are no special build rules so one could simply build by calling the compiler directly, refer to the Makefile for details.

The DeepView AI Middleware applications and libraries are intended to be easily integrated into various toolchains and build infrastructure.  We demonstrate building using our offered Yocto SDK, for support on alternative toolchains and build infrastructure contact Au-Zone Technologies.

## Command-Line

Install the Yocto SDK for VisionPack or a compatible Yocto toolchain, then compile with the following commands.

```shell
source /opt/yocto/environment
make
```

## Visual Studio Code

The project includes a [Visual Studio Code][vscode] configuration which uses our [Yocto SDK for VisionPack][yocto-sdk] container to enable building AI Middleware applications for various supported targets.

Open the project in Visual Studio Code on your desktop then select the "Open a Remote Window" option at the very bottom left of Visual Studio Code, next select "Reopen in Container".  When prompted to select a CMake kit, choose the appropriate "Yocto SDK for ..." option appropriate for your target.  Now you can work from Visual Studio Code and when building the application it will be correctly cross-compiled for the embedded Linux target platform.

# Camera Stream

Included in this repository is a camera.sh script which uses GStreamer to capture from a V4L2 camera into VSL which the detect application can use for capture.

You can run the script with the `--help` parameter for configuration options, especially relevant is the source camera device.  The script uses `/dev/video3` which is the default on the i.MX 8M Plus EVK with the OV5640 sensor.  If using the OS08 sensor with ISP the default should be `/dev/video2`.

# Video Stream

Included in this repository is a video.sh script which uses GStreamer to playback a pre-recorded MP4 video and inject into VSL which the detect application can use for capture.  This allows you to test the pipeline using pre-recorded videos, the WebVision service continues to stream this video to the browser.

For other video formats simply adjust the GStreamer pipeline contained within video.sh for your requirements.

[edgeai]: https://edgefirst.ai
[zeromq]: https://zeromq.org
[vscode]: https://code.visualstudio.com
[webvision]: https://github.com/DeepViewML/webvision
[yocto-sdk]: https://github.com/DeepViewML/yocto-sdk