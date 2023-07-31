# Stream Based General Detection
This code example demonstrates how to run general detection using VAAL. The C code in detectimg.c contains the sample application that runs general detection and demonstrates how it can be modified to fit your needs. This repo contains information on how to run this application on Maivin using Torizon, directly using Docker on Maivin, on an EVK as well as on one's desktop.

# Contents
[Workflow Documentation](#vaal-workflow)
[GStreamer Documentation](#gstreamer-integration)
[Quick Start](#quick-start)

## VAAL Workflow
When creating a VAAL Application, there are 3 stages involved, Initialization, Inference Loop, and Deallocation. The Inference Loop stage is composed of three main components, preprocessing, inference, and post-processing. We will examine each and provide a general overview and how to make modifications within those to tailor the application to one's parameters.

### Initialization Stage
This stage of the VAAL Workflow handles the creation of a context, the loading of a model as well as setting any additional parameters that may be necessary.

To create a context with a pre-determined model the following can be written as seen in detectimg.c lines 138-140
```
VAALContext* ctx = vaal_context_create(engine);
err = vaal_load_model_file(ctx, model);
```

The first line initializes the Context, a base structure used by VAAL for all matters related to the workflow. Please see additional documentation for further information. The engine parameter provided determines which Processing Unit to perform inference upon, with current support for CPU, GPU, and NPU.

The second line handles the loading and memory allocation for the RTM Model provided. See [here]() for further information on generating your own RTM Models.

Once these two lines have been run, your Context has been prepared and is ready for inference.

In some cases, particularly in this case of object detection, additional parameters can be held by the Context to provide information for pre and post-processing.

```
vaal_parameter_seti(ctx, "max_detection", &max_detection, 1);
vaal_parameter_setf(ctx, "score_threshold", &score_thr, 1);
vaal_parameter_setf(ctx, "iou_threshold", &iou_thr, 1);
vaal_parameter_seti(ctx, "normalization", &norm, 1);
```

The code above sets values for normalization of images in pre-processing as well as parameters that will be used for NMS post-processing of the detection boxes. These parameters are stored as a dictionary within the context, so any name can be stored, but only some are recognized. To see the full list of recognized parameters, follow this [link]().

Additionally, for initialization, depending on the type of model that is being used, a structure will need to be initialized to store the resultant post-processed information. These can be initialized as follows

```
VAALBox* boxes = calloc(max_detection, sizeof(VAALBox));
VAALKeypoint* keypoints = calloc(max_detection, sizeof(VAALKeypoint));
VAALEuler* orientations = calloc(max_detection, sizeof(VAALEuler));
```

These are the current three structures that are supported by VAAL and have their associated post-processing functions built-in to the VAAL Library. The VAALBox structure is used during object detection, it stores the box coordinates as well as the associated score and label. The VAALKeypoint structure is used during keypoint detection, finding specific keypoints on an object, for example, joints on a human body. A keypoint will store the x,y location, the score and label of a keypoint. The VAALEuler structure is used for pose estimation, with our most prominent use case, being head orientation. An Euler structure will contain the yaw, pitch, and roll information. Please see further [documentation]() for working with the data structures and creating code to suit your own needs to analayze the results stored within these data structures.

### Inference Loop Stage
The inference loop is the primary component of any VAAL Application and will be where any data analysis will want to be added after each inference is performed. This stage is responsible for the loading of the inputs/pre-processing, the inference of the model on that input, as well as any post-processing and analysis that is to be performed on the resultant data.

#### Loading/Pre-Processing
To prepare the input tensor for inference, we provide a simple function call to load images from file
```
err = vaal_load_image_file(ctx, NULL, image, NULL, 0);
```
Following this structure, this loads the image file, into the provided context. Looking at the [documentation]() we are able to provide an ROI for the image, should this become necessary in a multi-stage pipeline where the full image is not necessary as well as provide normalization information directly in the function call. From previously, we have seen that we are able to set the normalization parameter within the context. This will be used if the proc parameter is left as 0. As a warning, if a parameter is provided for normalization and the normalization parameter is set within the context, the library will perform a bitwise or of the two and it may lead to unexpected results, so it is recommended to use one or the other. To work with loading direct data, please see the documentation on [vaal_load_frame_memory](). When using GStreamer we will directly with the DMA Buffers and we will use the function [vaal_load_frame_dmabuf]().

#### Inference
While this stage does the majority of the heavy lifting, the coding of this step is extremely straightforward with a single function call, with only the context containing the model as an argument.
```
err = vaal_run_model(ctx);
```
When run, the model will use whatever data is stored as the input, so ensure that it has been loaded properly. The majority of RTM models are memory mapped to reduce the memory footprint on smaller hardware and this means that only the output layers are guaranteed to be saved at the end of inference and any intermediate layers, including the input, will be incoherent. Currently, there is no step function available through the VAAL Library, so if an intermediary layer is required after inference, please see the conversion documentation, further up, on how to preserve a layer's place in memory.

#### Post-Processing/Data Analysis
This component is the place where the majority of changes will take place and where one will want to modify the code most to suit their needs as every individual will have a different use for the resultant data, from displaying through text or video, to analysis, to sending it further along a pipeline for a multi-stage application.

For object detection, keypoint detection, and pose estimation, we provide structures as well as post-processing functions that are available directly through the VAAL Library. The creation of these structures has been noted above, but for using the post-processing functions, they can be called as follows
```
err = vaal_boxes(context, boxes, max_boxes, &num_boxes);
err = vaal_keypoints(context, keypoints, max_keypoints, &num_keypoints);
err = vaal_euler(context, orientations, &num_orientations);
```
These will populate the provided data structures with results from post-processing using the outputs of the model. As the data structures will be contained in a list, one will then be able to loop through the results, using the modified value of num_object, to look at each of the results found. For reference, here is an example of looping through the boxes found.
```
for (size_t j = 0; j < num_boxes; j++) {
    const VAALBox* box   = &boxes[j];
    const char*    label = vaal_label(ctx, box->label);
```
If you are using a model that does not use any of these post-procesing functions, you can access the outputs of the model using [vaal_output_tensor](). This can be used as follows
```
NNTensor* output = vaal_output_tensor(ctx, index);
```
At this point you are free to utilize the data as you need for whatever application you are developing, whether that be data from the post-processing functions provided in the VAAL Library or the direct outputs from the model.

### Deallocation Stage
This stage is relatively straightforward and does not have complicated use as it is responsible for the deallocation of memory to avoid any memory leaks within your application. As was seen in the Initialization Stage, there are two elements of a VAAL Workflow where memory is allocated with the Context as well as the data structures used to store post-processed information. These can be deallocated as seen through the following code snippet.
```
free(boxes);
free(keypoints);
free(orientations);
vaal_context_release(context);
```
The data structures can be deallocated as usual with free, but to ensure the proper release of the context, it is recommended to use the function vaal_context_release.

## GStreamer Integration
This application demonstrates how the VAAL Workflow can be integrated into a GStreamer pipeline. The majority of GStreamer code is boilerplate with the workflow being added in the function
```
static GstFlowReturn
new_sample(GstElement* sink, VAALContext* ctx)
```
This function is the final stage of the pipeline created
```
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
```
After some further initialization and setup of the GStreamer pipeline, the new_sample function is then associated with that element in the pipeline ```g_signal_connect(appsink, "new-sample", G_CALLBACK(new_sample), ctx);``` where the third argument sets the callback which is assigned the new_sample function, and any additional arguments beyond that, become additional argument values for the callback function assigned.

Within the new_sample function, the first step is to process the incoming stream of data to receive the DMA buffer which holds the current frame. The code for accessing this information is boilerplate for our applications so it is recommended to use it as-is. For further GStreamer documentation, see [here](). Once the DMA buffer has been processed, the new_sample function will enter into the same VAAL Workflow as described previously.

## Quick Start
### Maivin Using Torizon

#### Setup
1. Please see https://support.deepviewml.com/hc/en-us/articles/10977327933965-Visual-Studio-Torizon-Plugin for installing the extension and attaching your Maivin.
2. Once this has been setup, within VSCode navigate to Run -> Start Debugging and the project will be run on the Maivin.

#### Modifications
To change what model or images are used with the general detection app, on the left toolbar in VSCode, select the Torizon Extension. Go to the Configurations section and from here you can change what model or images are used. These new models and images must be provided in the appconfig_0 folder to be accessible through the Torizon extension.

### Maivin Using Docker on Target

### NXP i.MX 8M Plus EVK

### Desktop Linux

#### Setup
1. Please follow these [instructions](https://support.deepviewml.com/hc/en-us/articles/8328205801101) to install the necessary packages to build the application.
2. Ensure make is install ```sudo apt-get update && sudo apt-get install make``
3. Install the required GStreamer packages ```sudo apt-get install gstreamer-1.0 libgstreamer-plugins-base-1.0-dev libgstvideo-1.0```
4. In the base folder of this repo, run ```make```

At this point the detectimg application will be built and can be run using the provided samples or using your own model and image.
```
./detectimg -e cpu appconfig_0/mpk-coco-people.rtm appconfig_0/test_image.png
VisionPack 1.4.0 EVALUATION - Copyright 2022 Au-Zone Technologies
  [box] label            (scr%): xmin ymin xmax ymax  [    load    infer    boxes]
appconfig_0/test_image.png                            [   43.19  1056.70     1.96]
  [  0] person           ( 88%): 0.80 0.44 0.96 0.74
  [  1] person           ( 52%): 0.32 0.46 0.40 0.52
```
