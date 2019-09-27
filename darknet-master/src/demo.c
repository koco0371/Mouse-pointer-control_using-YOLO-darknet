#include "network.h"
#include "detection_layer.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "image.h"
#include "demo.h"

#include <X11/extensions/XTest.h> // control_display
#include <unistd.h> // control_display

#ifdef WIN32
#include <time.h>
#include "gettimeofday.h"
#else
#include <sys/time.h>
#endif



#ifdef OPENCV

#include "http_stream.h"

static char **demo_names;
static image **demo_alphabet;
static int demo_classes;

static int nboxes = 0;
static detection *dets = NULL;

static network net;
static image in_s;
static image det_s;

static cap_cv *cap;
static float fps = 0;
static float demo_thresh = 0;
static int demo_ext_output = 0;
static long long int frame_id = 0;
static int demo_json_port = -1;

#define NFRAMES 3

static float* predictions[NFRAMES];
static int demo_index = 0;
static image images[NFRAMES];
static mat_cv* cv_images[NFRAMES];
static float *avg;

mat_cv* in_img;
mat_cv* det_img;
mat_cv* show_img;

static volatile int flag_exit;
static int letter_box = 0;

/* this is our graduation project code*/
#define NOTHING -1
#define PALM 0
#define FIST 1

#define SCALE 100
#define FRAMECNT 10
#define DRAGCNT 1


int cur_state = NOTHING;
int prev_state = NOTHING;
int frame_count = FRAMECNT ;
int drag_count = DRAGCNT;
double prev_x = 0;
double prev_y = 0;
double cur_x = 0;
double cur_y = 0;
double palm_fist_dif=0;

double get_x_distance()
{
    return cur_x - prev_x;
}

double get_y_distance()
{
    return cur_y - prev_y>0.3?cur_y-prev_y-palm_fist_dif:cur_y-prev_y;
}

void left_down()
{
    Display *dpy = NULL;
    XEvent event;
    dpy = XOpenDisplay(NULL);


    XQueryPointer(dpy, RootWindow(dpy, 0), &event.xbutton.root,
        &event.xbutton.window, &event.xbutton.x_root,
        &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y,
        &event.xbutton.state);

    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, 0);
    XCloseDisplay(dpy);
}

void left_up()
{
    Display *dpy = NULL;
    XEvent event;
    dpy = XOpenDisplay(NULL);


    XQueryPointer(dpy, RootWindow(dpy, 0), &event.xbutton.root,
        &event.xbutton.window, &event.xbutton.x_root,
        &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y,
        &event.xbutton.state);

    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, 0);

    XCloseDisplay(dpy);

}

void left_click()
{
    if (prev_state == PALM && cur_state == FIST)
    {
        palm_fist_dif = get_y_distance();
        left_down();
    }
    else if (prev_state == FIST && cur_state == PALM)
    {
        
        left_up();
    }
    prev_state = cur_state;
    prev_x = cur_x;
    prev_y = cur_y;
}

void drag_fist()
{
    Display *dpy = NULL;
    XEvent event;
    dpy = XOpenDisplay(NULL);

    /* Get the current pointer position */
    XQueryPointer(dpy, RootWindow(dpy, 0), &event.xbutton.root,
        &event.xbutton.window, &event.xbutton.x_root,
        &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y,
        &event.xbutton.state);

    /* Fake the pointer movement to new relative position */
    XTestFakeMotionEvent(dpy, 0, event.xbutton.x +(get_x_distance()*SCALE*30), event.xbutton.y + get_y_distance()*SCALE*20, CurrentTime);
    XSync(dpy, 0);
    sleep(0.65);
    XCloseDisplay(dpy);

}

void move_pointer()
{
    Display *dpy = NULL;
    XEvent event;
    dpy = XOpenDisplay(NULL);

    /* Get the current pointer position */
    XQueryPointer(dpy, RootWindow(dpy, 0), &event.xbutton.root,
        &event.xbutton.window, &event.xbutton.x_root,
        &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y,
        &event.xbutton.state);
    
    //printf("event.xbutton.x, y : %f, %f\n", event.xbutton.x, event.xbutton.y);

    /* Fake the pointer movement to new relative position */
    XTestFakeMotionEvent(dpy, 0, event.xbutton.x +(get_x_distance()*SCALE*30), event.xbutton.y + get_y_distance()*SCALE*20, CurrentTime);
    XSync(dpy, 0);
    sleep(0.65);
    XCloseDisplay(dpy);

}

void drag()
{
    if (prev_state == FIST && cur_state == FIST)
    {
        if(drag_count==0){
            drag_count=DRAGCNT;    
            drag_fist();
        }
        else{
            drag_count-=1;
        }
    }
    else if (prev_state == PALM && cur_state == PALM)
    {
        move_pointer();
    }
    prev_state = cur_state;
    prev_x = cur_x;
    prev_y = cur_y;
}

void click_release() 
{
    if (prev_state == FIST) 
    {
        left_up();
    }
}

void detect_hand() {
    Display *dpy = NULL;
    XEvent event;
    dpy = XOpenDisplay(NULL);

    XQueryPointer(dpy, RootWindow(dpy, 0), &event.xbutton.root,
        &event.xbutton.window, &event.xbutton.x_root,
        &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y,
        &event.xbutton.state);
   
   // XTestFakeMotionEvent(dpy, 0, cur_x*SCALE*30, cur_y*SCALE*20, CurrentTime);
    XSync(dpy, 0);
    sleep(0.65);
    XCloseDisplay(dpy);
    prev_state = cur_state;
}


void control_display(detection* sorted_dets, float thresh, char** names, int classes, int num) 
{
    int i, j;
    int class_id;
    int flag = 0;
    //FILE * curout = fopen("curout.txt", "w");
    for (i = 0; i < num; ++i) 
    {
        class_id = -1;
        for (j = 0; j < classes; ++j) 
        {
            int show = strncmp(names[j], "dont_show", 9);
            if (sorted_dets[i].prob[j] > thresh && show) 
            {
                class_id = j;
                cur_x = sorted_dets[i].bbox.x;
                cur_y = sorted_dets[i].bbox.y;
                       
                //printf("cursor : %2.4f %2.4f\n", cur_x*SCALE, cur_y*SCALE);
                //printf ("box : 2.4f", sorted_dets[i].bbox.h);
                flag = 1;
                break;
            }
        }
        if (flag == 1)
            break;
    }
    cur_state = class_id;

    if (cur_state == NOTHING) 
    {
        
        if(frame_count==0) 
        {
       	    click_release();
       	    prev_state = NOTHING;
	        frame_count = FRAMECNT ;
            return;
        }
        else
        {
	        frame_count-=1;
        }

    }
    else if (cur_state != prev_state) 
    {
        if (prev_state == NOTHING && cur_state == PALM) 
        {
            detect_hand();
	        frame_count = FRAMECNT ;
        }
        else 
        {
            frame_count = FRAMECNT ;
            left_click();
        }
	        
    }
    else if (cur_state == prev_state) 
    {
	    frame_count = FRAMECNT ;
        drag();
    }
}
/* end */

void *fetch_in_thread(void *ptr)
{
    int dont_close_stream = 0;    // set 1 if your IP-camera periodically turns off and turns on video-stream
    if (letter_box)
        in_s = get_image_from_stream_letterbox(cap, net.w, net.h, net.c, &in_img, dont_close_stream);
    else
        in_s = get_image_from_stream_resize(cap, net.w, net.h, net.c, &in_img, dont_close_stream);
    if (!in_s.data) {
        printf("Stream closed.\n");
        flag_exit = 1;
        //exit(EXIT_FAILURE);
        return 0;
    }
    //in_s = resize_image(in, net.w, net.h);

    return 0;
}

void *detect_in_thread(void *ptr)
{
    layer l = net.layers[net.n - 1];
    float *X = det_s.data;
    float *prediction = network_predict(net, X);

    memcpy(predictions[demo_index], prediction, l.outputs * sizeof(float));
    mean_arrays(predictions, NFRAMES, l.outputs, avg);
    l.output = avg;

    free_image(det_s);

    cv_images[demo_index] = det_img;
    det_img = cv_images[(demo_index + NFRAMES / 2 + 1) % NFRAMES];
    demo_index = (demo_index + 1) % NFRAMES;

    if (letter_box)
        dets = get_network_boxes(&net, get_width_mat(in_img), get_height_mat(in_img), demo_thresh, demo_thresh, 0, 1, &nboxes, 1); // letter box
    else
        dets = get_network_boxes(&net, net.w, net.h, demo_thresh, demo_thresh, 0, 1, &nboxes, 0); // resized

    return 0;
}

double get_wall_time()
{
    struct timeval walltime;
    if (gettimeofday(&walltime, NULL)) {
        return 0;
    }
    return (double)walltime.tv_sec + (double)walltime.tv_usec * .000001;
}

void demo(char *cfgfile, char *weightfile, float thresh, float hier_thresh, int cam_index, const char *filename, char **names, int classes,
    int frame_skip, char *prefix, char *out_filename, int mjpeg_port, int json_port, int dont_show, int ext_output, int letter_box_in)
{
    FILE* temptestout = fopen("temptestout.txt", "w");
    letter_box = letter_box_in;
    in_img = det_img = show_img = NULL;
    //skip = frame_skip;

    image **alphabet = load_alphabet();
    int delay = frame_skip;
    demo_names = names;
    demo_alphabet = alphabet;
    demo_classes = classes;
    demo_thresh = thresh;
    demo_ext_output = ext_output;
    demo_json_port = json_port;
    printf("Demo\n");
    net = parse_network_cfg_custom(cfgfile, 1, 1);    // set batch=1
    if (weightfile) {
        load_weights(&net, weightfile);
    }
    fuse_conv_batchnorm(net);
    calculate_binary_weights(net);
    srand(2222222);

    if (filename) {
        printf("video file: %s\n", filename);
        cap = get_capture_video_stream(filename);
    }
    else {
        printf("Webcam index: %d\n", cam_index);
        cap = get_capture_webcam(cam_index);
    }

    if (!cap) {
#ifdef WIN32
        printf("Check that you have copied file opencv_ffmpeg340_64.dll to the same directory where is darknet.exe \n");
#endif
        error("Couldn't connect to webcam.\n");
    }

    layer l = net.layers[net.n - 1];
    int j;

    avg = (float *)calloc(l.outputs, sizeof(float));
    for (j = 0; j < NFRAMES; ++j) predictions[j] = (float *)calloc(l.outputs, sizeof(float));
    for (j = 0; j < NFRAMES; ++j) images[j] = make_image(1, 1, 3);

    if (l.classes != demo_classes) {
        printf("Parameters don't match: in cfg-file classes=%d, in data-file classes=%d \n", l.classes, demo_classes);
        getchar();
        exit(0);
    }


    flag_exit = 0;

    pthread_t fetch_thread;
    pthread_t detect_thread;

    fetch_in_thread(0);
    det_img = in_img;
    det_s = in_s;

    fetch_in_thread(0);
    detect_in_thread(0);
    det_img = in_img;
    det_s = in_s;

    for (j = 0; j < NFRAMES / 2; ++j) {
        fetch_in_thread(0);
        detect_in_thread(0);
        det_img = in_img;
        det_s = in_s;
    }

    int count = 0;
    if (!prefix && !dont_show) {
        int full_screen = 0;
        create_window_cv("Demo", full_screen, 427, 240/*1352, 1013*/); // window size modified
    }


    write_cv* output_video_writer = NULL;
    if (out_filename && !flag_exit)
    {
        int src_fps = 25;
        src_fps = get_stream_fps_cpp_cv(cap);
        output_video_writer =
            create_video_writer(out_filename, 'D', 'I', 'V', 'X', src_fps, get_width_mat(det_img), get_height_mat(det_img), 1);

        //'H', '2', '6', '4'
        //'D', 'I', 'V', 'X'
        //'M', 'J', 'P', 'G'
        //'M', 'P', '4', 'V'
        //'M', 'P', '4', '2'
        //'X', 'V', 'I', 'D'
        //'W', 'M', 'V', '2'
    }

    double before = get_wall_time();

    while (1) {
        ++count;
        {
            if (pthread_create(&fetch_thread, 0, fetch_in_thread, 0)) error("Thread creation failed");
            if (pthread_create(&detect_thread, 0, detect_in_thread, 0)) error("Thread creation failed");

            float nms = .45;    // 0.4F
            int local_nboxes = nboxes;
            detection *local_dets = dets;

            //if (nms) do_nms_obj(local_dets, local_nboxes, l.classes, nms);    // bad results
            if (nms) do_nms_sort(local_dets, local_nboxes, l.classes, nms);
            if (count & 15 == 15)
                control_display(local_dets, demo_thresh, demo_names, demo_classes, local_nboxes);

            //print class!!
            /*int i;
            for (i = 0; i < local_nboxes; ++i) {
                int class_id = -1;
                float prob = 0;
                for (j = 0; j < l.classes; ++j) {
                    if (local_dets[i].prob[j] > thresh && local_dets[i].prob[j] > prob) {
                        if (class_id < 0) {
                            class_id = j;
                            fprintf(temptestout, "class name : %d\n", class_id);
                            fprintf(temptestout, "prob : %f\n", local_dets[i].prob[class_id]);
                            fprintf(temptestout, "%2.4f %2.4f %2.4f %2.4f\n", local_dets[i].bbox.x, local_dets[i].bbox.y);
                        }
                        else {
                            fprintf(temptestout, "+\n");
                            fprintf(temptestout, "class name : %d\n", j);
                            fprintf(temptestout, "prob : %f\n", local_dets[i].prob[j]);
                            fprintf(temptestout, "%2.4f %2.4f %2.4f %2.4f\n", local_dets[i].bbox.x, local_dets[i].bbox.y);
                        }
                    }
                }
            }
            if (local_nboxes != 0) fprintf(temptestout, "---------------\n");*/


            //printf("\033[2J");
            //printf("\033[1;1H");
            //printf("\nFPS:%.1f\n", fps);
            printf("Objects:\n\n");

            ++frame_id;
            if (demo_json_port > 0) {
                int timeout = 400000;
                send_json(local_dets, local_nboxes, l.classes, demo_names, frame_id, demo_json_port, timeout);
            }

            draw_detections_cv_v3(show_img, local_dets, local_nboxes, demo_thresh, demo_names, demo_alphabet, demo_classes, demo_ext_output);
            free_detections(local_dets, local_nboxes);

            printf("\nFPS:%.1f\n", fps);

            if (!prefix) {
                if (!dont_show) {
                    show_image_mat(show_img, "Demo");
                    int c = wait_key_cv(1);
                    if (c == 10) {
                        if (frame_skip == 0) frame_skip = 60;
                        else if (frame_skip == 4) frame_skip = 0;
                        else if (frame_skip == 60) frame_skip = 4;
                        else frame_skip = 0;
                    }
                    else if (c == 27 || c == 1048603) // ESC - exit (OpenCV 2.x / 3.x)
                    {
                        flag_exit = 1;
                    }
                }
            }
            else {
                char buff[256];
                sprintf(buff, "%s_%08d.jpg", prefix, count);
                if (show_img) save_cv_jpg(show_img, buff);
            }

            // if you run it with param -mjpeg_port 8090  then open URL in your web-browser: http://localhost:8090
            if (mjpeg_port > 0 && show_img) {
                int port = mjpeg_port;
                int timeout = 400000;
                int jpeg_quality = 40;    // 1 - 100
                send_mjpeg(show_img, port, timeout, jpeg_quality);
            }

            // save video file
            if (output_video_writer && show_img) {
                write_frame_cv(output_video_writer, show_img);
                printf("\n cvWriteFrame \n");
            }

            release_mat(&show_img);

            pthread_join(fetch_thread, 0);
            pthread_join(detect_thread, 0);

            if (flag_exit == 1) break;

            if (delay == 0) {
                show_img = det_img;
            }
            det_img = in_img;
            det_s = in_s;
        }
        --delay;
        if (delay < 0) {
            delay = frame_skip;

            //double after = get_wall_time();
            //float curr = 1./(after - before);
            double after = get_time_point();    // more accurate time measurements
            float curr = 1000000. / (after - before);
            fps = curr;
            before = after;
        }
    }
    printf("input video stream closed. \n");
    if (output_video_writer) {
        release_video_writer(&output_video_writer);
        printf("output_video_writer closed. \n");
    }

    // free memory
    release_mat(&show_img);
    release_mat(&in_img);
    free_image(in_s);

    free(avg);
    for (j = 0; j < NFRAMES; ++j) free(predictions[j]);
    for (j = 0; j < NFRAMES; ++j) free_image(images[j]);

    free_ptrs((void **)names, net.layers[net.n - 1].classes);

    int i;
    const int nsize = 8;
    for (j = 0; j < nsize; ++j) {
        for (i = 32; i < 127; ++i) {
            free_image(alphabet[j][i]);
        }
        free(alphabet[j]);
    }
    free(alphabet);
    free_network(net);
    //cudaProfilerStop();
}
#else
void demo(char *cfgfile, char *weightfile, float thresh, float hier_thresh, int cam_index, const char *filename, char **names, int classes,
    int frame_skip, char *prefix, char *out_filename, int mjpeg_port, int json_port, int dont_show, int ext_output, int letter_box_in)
{
    fprintf(stderr, "Demo needs OpenCV for webcam images.\n");
}
#endif
