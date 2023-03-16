/**
 * Copyright 2022 by Au-Zone Technologies.  All Rights Reserved.
 *
 * Software that is described herein is for illustrative purposes only which
 * provides customers with programming information regarding the DeepView VAAL
 * library. This software is supplied "AS IS" without any warranties of any
 * kind, and Au-Zone Technologies and its licensor disclaim any and all
 * warranties, express or implied, including all implied warranties of
 * merchantability, fitness for a particular purpose and non-infringement of
 * intellectual property rights.  Au-Zone Technologies assumes no responsibility
 * or liability for the use of the software, conveys no license or rights under
 * any patent, copyright, mask work right, or any other intellectual property
 * rights in or to any products. Au-Zone Technologies reserves the right to make
 * changes in the software without notification. Au-Zone Technologies also makes
 * no representation or warranty that such application will be suitable for the
 * specified use without further testing or modification.
 */

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deepview_rt.h>
#include <vaal.h>
#include <videostream.h>
#include <zmq.h>

#include "json.hpp"
#include "zmq.hpp"

#include "version.h"

#define USEC_PER_SEC 1000000ll
#define NSEC_PER_SEC (1000ll * USEC_PER_SEC)

using namespace std::chrono;
using nlohmann::json;

namespace data
{
struct box {
    float xmin;
    float xmax;
    float ymin;
    float ymax;
};

struct object {
    std::string label;
    float       score;
    box         bbox;
};

struct result {
    int64_t             timestamp;
    int                 fps;
    int64_t             load_ns;
    int64_t             model_ns;
    int64_t             boxes_ns;
    std::vector<object> objects;
};

struct capture {
    int64_t timestamp;
    int64_t serial;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(box, xmin, xmax, ymin, ymax)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(object, bbox, score, label)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(result,
                                                timestamp,
                                                fps,
                                                load_ns,
                                                model_ns,
                                                boxes_ns,
                                                objects)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(capture, timestamp, serial)

} // namespace data

static int        running = 1;
static int        verbose = 0;
static VSLClient* vsl     = NULL;

static int
update_fps()
{
    static system_clock::time_point previous_time;
    static int                      fps_history[30] = {0};
    static int                      fps_index       = 0;

    auto timestamp         = system_clock::now();
    auto frame_time        = timestamp - previous_time;
    previous_time          = timestamp;
    fps_history[fps_index] = NSEC_PER_SEC / frame_time.count();
    fps_index              = fps_index >= 29 ? 0 : fps_index + 1;

    int fps = 0;
    for (int i = 0; i < 30; i++) { fps += fps_history[i]; }
    fps /= 30;

    return fps;
}

/**
 * On sigint we set running to 0 (stop the event loop) and disconnect the vsl
 * client socket causing outstanding vsl_frame_wait() to terminate.
 */
static void
quit(int signum)
{
    (void) signum;

    running = 0;
    vsl_client_disconnect(vsl);
}

/**
 * This function is where we read the videostream frame and do perform model
 * inferencing with VisionPack VAAL.
 */
static int
handle_vsl(zmq::socket_t&        pub,
           const std::string&    topic,
           const std::string&    capture,
           VAALContext*          vaal,
           std::vector<VAALBox>& boxes)
{
    int     err;
    int64_t start;
    size_t  sz  = 0;
    char*   buf = NULL;

    /**
     * The vsl_frame_wait function will block until the next frame is received.
     *
     * IMPORTANT: vsl_frame_release must be called on the VSLFrame returned by
     * this function.  Failure to do so will result in leaked file descriptors
     * and the eventual termination of the application by the operating system.
     */
    VSLFrame* frame = vsl_frame_wait(vsl, 0);
    if (!frame) { return 0; }

    /**
     * The vsl_frame_trylock will attempt to lock the frame so that it can live
     * longer than the default lifespan, typically 100ms. It is technically not
     * needed in this case as the vaal_load_frame function will complete well
     * within the default lifespan of the frame as load_frame will complete in
     * under 5ms.  The trylock is included of illustrative purposes for cases
     * where the frame could be used beyond the default 100ms lifespan.
     */
    err = vsl_frame_trylock(frame);
    if (err) {
        fprintf(stderr, "failed to lock frame: %s\n", strerror(errno));
        vsl_frame_release(frame);
        return 0;
    }

    auto fps       = update_fps();
    auto timestamp = vsl_frame_timestamp(frame);

    /**
     * If capture is set then we need to publish a capture event with timestamp
     * and frame serial so that other services, such as image logging, can be
     * synchronized with the model frame capture.
     */
    if (capture.size()) {
        json payload = data::capture{
            .timestamp = timestamp,
            .serial    = vsl_frame_serial(frame),
        };
        auto message = capture + payload.dump(4);
        if (verbose) { std::cout << message << std::endl; }
        pub.send(zmq::buffer(message));
    }

    start = vaal_clock_now();
    err   = vaal_load_frame_dmabuf(vaal,
                                 NULL,
                                 vsl_frame_handle(frame),
                                 vsl_frame_fourcc(frame),
                                 vsl_frame_width(frame),
                                 vsl_frame_height(frame),
                                 NULL,
                                 0);
    vsl_frame_unlock(frame);
    vsl_frame_release(frame);

    if (err) {
        fprintf(stderr,
                "failed to load frame into model: %s\n",
                vaal_strerror(VAALError(err)));
        return -1;
    }

    int64_t load_ns = vaal_clock_now() - start;

    start = vaal_clock_now();
    err   = vaal_run_model(vaal);
    if (err) {
        fprintf(stderr,
                "failed to run model: %s\n",
                vaal_strerror(VAALError(err)));
        return -1;
    }
    int64_t model_ns = vaal_clock_now() - start;

    /**
     * The vaal_boxes function will load our array of VAALBox structures with
     * the bounding boxes identified by the model output.  The max_boxes
     * parameter is the size of the boxes array while the n_boxes is updated by
     * vaal_boxes with the actual number of bounding boxes loaded into the boxes
     * array, in other words the number of box detections from this inference.
     *
     * The vaal_boxes function internally handles the model output box decoding
     * and nms.
     */
    start = vaal_clock_now();
    size_t n_boxes;
    err = vaal_boxes(vaal, boxes.data(), boxes.size(), &n_boxes);
    if (err) {
        fprintf(stderr,
                "failed to read bounding boxes from model: %s\n",
                vaal_strerror(VAALError(err)));
        return -1;
    }
    int64_t boxes_ns = vaal_clock_now() - start;

    /**
     * The following code generates a JSON structure with the inference results.
     * The model and timing information is populated into fields of the root
     * object then an array of detected boxes is populated.
     */
    data::result result = {
        .timestamp = timestamp,
        .fps       = fps,
        .load_ns   = load_ns,
        .model_ns  = model_ns,
        .boxes_ns  = boxes_ns,
    };

    for (int i = 0; i < n_boxes; i++) {
        const VAALBox* box   = &boxes[i];
        const char*    label = vaal_label(vaal, box->label);

        result.objects.push_back({
            .label = label ? label : "",
            .score = box->score,
            .bbox =
                {
                    .xmin = box->xmin,
                    .xmax = box->xmax,
                    .ymin = box->ymin,
                    .ymax = box->ymax,
                },
        });
    }

    json payload = result;
    auto message = topic + payload.dump(4);
    if (verbose) { std::cout << message << std::endl; }
    pub.send(zmq::buffer(message));

    return 0;
}

int
main(int argc, char** argv)
{
    int         err;
    int         max_boxes = 50;
    float       threshold = 0.5f;
    float       iou       = 0.5f;
    const char* engine    = "npu";
    const char* vslpath   = "/tmp/camera.vsl";
    const char* puburl    = "ipc:///tmp/detect.pub";
    std::string topic     = "DETECTION";
    std::string capture   = "";

    struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"verbose", no_argument, NULL, 'v'},
        {"engine", required_argument, NULL, 'e'},
        {"vsl", required_argument, NULL, 's'},
        {"pub", required_argument, NULL, 'p'},
        {"topic", required_argument, NULL, 't'},
        {"capture-topic", required_argument, NULL, 'c'},
        {"max-boxes", required_argument, NULL, 'm'},
        {"threshold", required_argument, NULL, 'T'},
        {"iou", required_argument, NULL, 'I'},
        {NULL},
    };

    for (;;) {
        int opt = getopt_long(argc, argv, "hVve:m:s:p:t:c:T:I:", options, NULL);
        if (opt == -1) break;

        switch (opt) {
        case 'h':
            printf("detect [OPTIONS] MODEL\n"
                   "-h, --help\n"
                   "    display help information\n"
                   "-V, --version\n"
                   "    display version information\n"
                   "-v, --verbose\n"
                   "   enable verbose logging of each message\n"
                   "-m MAX --max-boxes MAX\n"
                   "    maximum detection boxes per frame (default: %d)\n"
                   "-T THRESHOLD, --threshold THRESHOLD\n"
                   "    set the detection threshold (default: %.2f)\n"
                   "-I IOU, --iou IOU\n"
                   "    set the detection iou for nms (default: %.02f)\n"
                   "-e ENGINE, --engine ENGINE\n"
                   "    select the inference engine device [cpu, gpu, npu*]\n"
                   "-s PATH, --vsl PATH\n"
                   "    vsl socket path to capture frames (default: %s)\n"
                   "-p URL, --pub URL\n"
                   "    url for the result message queue (default: %s)\n"
                   "-t TOPIC, --topic TOPIC\n"
                   "    subscribe to publisher topic (default: '%s')\n"
                   "-c TOPIC, --capture TOPIC\n"
                   "    publish capture event to TOPIC when frame is loaded\n",
                   max_boxes,
                   threshold,
                   iou,
                   vslpath,
                   puburl,
                   topic.c_str());
            return EXIT_SUCCESS;
        case 'V':
            printf("detect %s\n", VERSION);
            return EXIT_SUCCESS;
        case 'v':
            verbose = 1;
            break;
        case 'e':
            engine = optarg;
            break;
        case 'm':
            max_boxes = atoi(optarg);
            break;
        case 'T':
            threshold = atof(optarg);
            break;
        case 't':
            topic = optarg;
            break;
        case 'c':
            capture = optarg;
            break;
        case 's':
            vslpath = optarg;
            break;
        case 'p':
            puburl = optarg;
            break;
        default:
            fprintf(stderr,
                    "invalid parameter %c, try --help for usage\n",
                    opt);
            return EXIT_FAILURE;
        }
    }

    if (argv[optind] == NULL) {
        fprintf(stderr, "missing required model, try --help for usage\n");
        return EXIT_FAILURE;
    }

    const char* model = argv[optind++];

    /**
     * The VAALContext is used for all VAAL operations and one should be created
     * per-model to be executed by the application.
     */
    auto vaal = vaal_context_create(engine);
    if (!vaal) {
        fprintf(stderr, "failed to create vaal context\n");
        return EXIT_FAILURE;
    }

    err = vaal_load_model_file(vaal, model);
    if (err) {
        fprintf(stderr,
                "failed to load %s: %s\n",
                model,
                vaal_strerror(VAALError(err)));
        return EXIT_FAILURE;
    }

    vaal_parameter_setf(vaal, "score_threshold", &threshold, 1);
    vaal_parameter_setf(vaal, "iou_threshold", &iou, 1);
    vaal_parameter_sets(vaal, "nms_type", "standard", 0);
    vaal_parameter_seti(vaal, "max_detection", &max_boxes, 1);

    std::vector<VAALBox> boxes(max_boxes);

    /**
     * The ZeroMQ Context is required for all ZeroMQ API functions.  We create
     * the context then we create our publisher socket which will be used for
     * publishing detection results from VAAL.
     */
    zmq::context_t ctx;
    zmq::socket_t  pub(ctx, zmq::socket_type::pub);
    pub.set(zmq::sockopt::conflate, 1);
    pub.set(zmq::sockopt::rcvhwm, 1);
    pub.bind(puburl);

    if (verbose) {
        printf("publishing results to [%s]: %s\n", topic.c_str(), puburl);
    }

    /**
     * The application uses the VideoStream Library for sharing camera frames
     * between the various applications for this demonstration.  We initialize
     * the client to connect to vslpath which should be the end-point into which
     * we inject capture frames using GStreamer and vslsink or a native vslhost
     * application.
     */
    vsl = vsl_client_init(vslpath, NULL, true);
    if (!vsl) {
        fprintf(stderr,
                "failed to connect videostream socket %s: %s\n",
                vslpath,
                strerror(errno));
        return EXIT_FAILURE;
    }

    if (verbose) { printf("capturing frames from %s\n", vslpath); }

    // 100ms timeout on frame capture.
    vsl_client_set_timeout(vsl, 0.1f);

    /**
     * Install a SIGINT handler so we can cleanup on a control-c keyboard input.
     */
    signal(SIGINT, quit);

    /**
     * There's many different ways for an application to implement its event
     * loop.  Here have a function which reads frames from vsl to send to the
     * model to perform inference then finally publishes results as JSON over
     * the ZeroMQ socket.  The application loop simply runs this function
     * forever.
     */
    while (running) {
        err = handle_vsl(pub, topic, capture, vaal, boxes);
        if (err) { return EXIT_FAILURE; }
    }

    /**
     * Cleanup resources before exiting the application.  This allows us to use
     * something like valgrind to ensure the application has no resource leaks.
     *
     * NOTE: the OpenVX driver required by NPU support will produce a lot of
     * valgrind noise, use the CPU for inference if you wish to test your
     * application for resource leaks.
     */
    vaal_context_release(vaal);

    return EXIT_SUCCESS;
}
