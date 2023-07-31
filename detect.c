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

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gst/allocators/allocators.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <vaal.h>

#define MICROS_PER_SEC 1000000ll /* microseconds per second */
#define NANOS_PER_SEC (1000ll * MICROS_PER_SEC)

#define array_sizeof(x) (sizeof(x) / sizeof(*x))

static GMainLoop* loop = NULL;

static void
quit(int signum)
{
    (void) signum;
    g_main_loop_quit(loop);
}

static int64_t
clock_now()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * NANOS_PER_SEC + now.tv_nsec;
}

static gboolean
on_message(GstBus* bus, GstMessage* message, gpointer data)
{
    GError* err = NULL;

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(message, &err, NULL);
        fprintf(stderr, "ERROR %d: %s\n", err->code, err->message);
        g_error_free(err);
        g_main_loop_quit(loop);
        break;
    case GST_MESSAGE_WARNING:
        gst_message_parse_warning(message, &err, NULL);
        fprintf(stderr, "WARNING %d: %s\n", err->code, err->message);
        g_error_free(err);
        break;
    default:
        break;
    }

    return TRUE;
}

static GstFlowReturn
new_sample(GstElement* sink, VAALContext* ctx)
{
    int        err;
    int        max_label = 16;
    GstSample* sample    = NULL;
    VAALBox    boxes[16] = {0};

    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) { return GST_FLOW_ERROR; }

    int64_t start = clock_now();
    GstCaps* caps  = gst_sample_get_caps(sample);

    gint          width, height;
    GstStructure* structure = gst_caps_get_structure(caps, 0);
    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);

    const char*    format;
    uint32_t       fourcc;
    GstVideoFormat videoformat;
    format      = gst_structure_get_string(structure, "format");
    videoformat = gst_video_format_from_string(format);
    fourcc      = gst_video_format_to_fourcc(videoformat);
    if (!fourcc) {
        fprintf(stderr, "empty fourcc\n");
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        fprintf(stderr, "sample has no buffer\n");
        return GST_FLOW_ERROR;
    }

    GstMemory* memory = gst_buffer_get_all_memory(buffer);
    if (!memory) {
        fprintf(stderr, "buffer has no memory\n");
        return GST_FLOW_ERROR;
    }

    if (!gst_is_dmabuf_memory(memory)) {
        fprintf(stderr, "memory is not dmabuf\n");
        return GST_FLOW_ERROR;
    }

    int dmabuf = gst_dmabuf_memory_get_fd(memory);

    err = vaal_load_frame_dmabuf(ctx,
                                 NULL,
                                 dmabuf,
                                 fourcc,
                                 width,
                                 height,
                                 NULL,
                                 0);
    if (err) {
        fprintf(stderr, "failed to load frame: %s\n", vaal_strerror(err));
        return GST_FLOW_ERROR;
    }

    gst_memory_unref(memory);
    gst_sample_unref(sample);

    int64_t load_ns = clock_now() - start;

    start = clock_now();
    err   = vaal_run_model(ctx);

    if (err) {
        fprintf(stderr, "failed to run model: %s\n", vaal_strerror(err));
        return GST_FLOW_ERROR;
    }
    int64_t inference_ns = clock_now() - start;

    start            = clock_now();
    int num_boxes    = 0;
    err              = vaal_boxes(ctx, boxes, array_sizeof(boxes), &num_boxes);
    int64_t boxes_ns = clock_now() - start;

    if (err) {
        fprintf(stderr, "failed to run boxes: %s\n", vaal_strerror(err));
        return GST_FLOW_ERROR;
    }

    printf("load: %8.2f inference: %8.2f boxes: %8.2f\n",
           load_ns / 1e6,
           inference_ns / 1e6,
           boxes_ns / 1e6);

    // Iterate over the boxes skipping the background at index 0.
    for (size_t j = 1; j < num_boxes; j++) {
        char           label_index[12];
        const VAALBox* box   = &boxes[j];
        const char*    label = vaal_label(ctx, box->label);

        if (!label) {
            snprintf(label_index, sizeof(label_index), "%d", box->label);
            label = label_index;
        }

        printf("    [%3zu] %-*s (%3d%%): %3.2f %3.2f %3.2f %3.2f\n",
               j,
               max_label,
               label,
               (int) (box->score * 100),
               box->xmin,
               box->ymin,
               box->xmax,
               box->ymax);
    }

    return GST_FLOW_OK;
}

int
main(int argc, char* argv[])
{
    const char* engine    = "npu";
    const char* camera    = "/dev/video3";
    const char* model     = NULL;
    const char* size      = NULL;
    const char* nms       = "standard";
    float       threshold = 0.5f;
    int         norm      = 0;

    static struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {"engine", required_argument, NULL, 'e'},
        {"camera", required_argument, NULL, 'c'},
        {"size", required_argument, NULL, 's'},
        {"threshold", required_argument, NULL, 't'},
        {"norm", required_argument, NULL, 'N'},
        {NULL, 0, NULL, 0},
    };

    for (;;) {
        int opt = getopt_long(argc, argv, "hve:c:s:t:N:", options, NULL);
        if (opt == -1) break;

        switch (opt) {
        case 'h':
            printf("usage: gst-detect "
                   "[hv] [-e ENGINE] [-c DEVICE] [-s WIDTHxHEIGHT] <MODEL>\n"
                   "-h\n"
                   "    Display help information and exit.\n"
                   "-e ENGINE, --engine ENGINE\n"
                   "    Use DeepViewRT ENGINE cpu, gpu, or npu (default: %s)\n"
                   "-c DEVICE, --camera DEVICE\n"
                   "    Video4Linux2 camera device (default: %s)\n"
                   "-s WIDTHxHEIGHT, --size WIDTHxHEIGHT\n"
                   "    Request camera to run at WIDTHxHEIGHT size\n"
                   "-t N, --threshold N\n"
                   "    Threshold for acceptable boxes (default: %.02f)\n"
                   "-n N, --nms N\n"
                   "    NMS method to use (standard*, matrix, fast)\n",
                   engine,
                   camera,
                   threshold);
            return EXIT_SUCCESS;
        case 'v':
            printf("DeepView VisionPack Detection Sample with VAAL %s\n",
                   vaal_version(NULL, NULL, NULL, NULL));
            return EXIT_SUCCESS;
        case 's':
            size = optarg;
            break;
        case 't':
            threshold = CLAMP(atof(optarg), 0.0f, 1.0f);
            break;
        case 'N':
            if (strcmp(optarg, "raw") == 0) {
                norm = 0;
            } else if (strcmp(optarg, "signed") == 0) {
                norm = VAAL_IMAGE_PROC_SIGNED_NORM;
            } else if (strcmp(optarg, "unsigned") == 0) {
                norm = VAAL_IMAGE_PROC_UNSIGNED_NORM;
            } else if (strcmp(optarg, "whitening") == 0) {
                norm = VAAL_IMAGE_PROC_WHITENING;
            } else if (strcmp(optarg, "imagenet") == 0) {
                norm = VAAL_IMAGE_PROC_IMAGENET;
            } else {
                fprintf(stderr,
                        "unsupported image normalization method: %s\n",
                        optarg);
                return EXIT_FAILURE;
            }

            break;
        case 'e':
            engine = optarg;
            break;
        case 'c':
            camera = optarg;
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

    model = argv[optind++];

    VAALContext* ctx = vaal_context_create(engine);

    // Load model from file
    int err = vaal_load_model_file(ctx, model);
    if (err) {
        fprintf(stderr, "failed to load model: %s\n", vaal_strerror(err));
        return EXIT_FAILURE;
    }

    vaal_parameter_sets(ctx, "nms_type", nms, 0);
    vaal_parameter_setf(ctx, "score_threshold", &threshold, 1);
    vaal_parameter_seti(ctx, "normalization", &norm, 1);

    /* Initialize GStreamer */
    gst_init(&argc, &argv);

    loop = g_main_loop_new(NULL, FALSE);

    if (!loop) {
        fprintf(stderr, "failed to create main loop: out of memory\n");
        return EXIT_FAILURE;
    }

    /* Build the pipeline */
    GstElement* pipeline = gst_pipeline_new("visionpack-detection");
    GstBus*     bus      = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, on_message, NULL);
    gst_object_unref(bus);

    GstElement* source = gst_element_factory_make("v4l2src", "source");
    if (!gst_bin_add(GST_BIN(pipeline), source)) {
        fprintf(stderr, "failed to add element v4l2src to pipeline\n");
        return EXIT_FAILURE;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", "filter");
    if (!gst_bin_add(GST_BIN(pipeline), filter)) {
        fprintf(stderr, "failed to add element capsfilter to pipeline\n");
        return EXIT_FAILURE;
    }

    GstElement* queue = gst_element_factory_make("queue", "queue");
    if (!gst_bin_add(GST_BIN(pipeline), queue)) {
        fprintf(stderr, "failed to add element queue to pipeline\n");
        return EXIT_FAILURE;
    }

    GstElement* appsink = gst_element_factory_make("appsink", "appsink");
    if (!gst_bin_add(GST_BIN(pipeline), appsink)) {
        fprintf(stderr, "failed to add element appsink to pipeline\n");
        return EXIT_FAILURE;
    }

    // We force io-mode=4 to ensure we get dmabuf backed memory from the camera.
    g_object_set(source, "device", camera, "io-mode", 4, NULL);
    g_object_set(appsink,
                 "sync",
                 TRUE,
                 "drop",
                 TRUE,
                 "max-buffers",
                 1,
                 "emit-signals",
                 TRUE,
                 NULL);

    if (size) {
        int width, height;
        sscanf(size, "%dx%d", &width, &height);
        GstCaps* filter_caps = gst_caps_new_simple("video/x-raw",
                                                   "width",
                                                   G_TYPE_INT,
                                                   width,
                                                   "height",
                                                   G_TYPE_INT,
                                                   height,
                                                   NULL);
        g_object_set(filter, "caps", filter_caps, NULL);
        gst_caps_unref(filter_caps);
    }

    if (!gst_element_link(source, filter)) {
        fprintf(stderr, "failed to link source to filter\n");
        return EXIT_FAILURE;
    }

    if (!gst_element_link(filter, queue)) {
        fprintf(stderr, "failed to link filter to queue\n");
        return EXIT_FAILURE;
    }

    if (!gst_element_link(queue, appsink)) {
        fprintf(stderr, "failed to link queue to appsink\n");
        return EXIT_FAILURE;
    }

    g_signal_connect(appsink, "new-sample", G_CALLBACK(new_sample), ctx);

    signal(SIGINT, quit);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_main_loop_unref(loop);

    return EXIT_SUCCESS;
}