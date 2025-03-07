/* Edge Impulse Arduino examples
/* Includes ---------------------------------------------------------------- */
// #include <smartpillbox_inferencing.h>
#include <vector>

#include <datasetchange_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

#include "edge-impulse-sdk/classifier/ei_classifier_types.h"

#include "esp_camera.h"

#include <Adafruit_NeoPixel.h>
#define PIN        12
#define NUMPIXELS 16
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);
#define DELAYVAL 30

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

/* Constant defines -------------------------------------------------------- */
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS           1024
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS           768
#define EI_CAMERA_FRAME_BYTE_SIZE                 3

/* Private variables ------------------------------------------------------- */
static bool debug_nn = false; // Set this to true to see e.g. features generated from the raw signal
static bool is_initialised = false;
uint8_t *snapshot_buf; //points to the output of the capture

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,

    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_XGA,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 8, //0-63 lower number means higher quality
    .fb_count = 1,       //if more than one, i2s runs in continuous mode. Use only with JPEG
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

/* Function definitions ------------------------------------------------------- */
bool ei_camera_init(void);
void ei_camera_deinit(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) ;

// 座標結構:
struct points {
  int x;
  int y;
}; 

// 藥盒格子定義位置:
std::vector<points> ROI={
  {124,477},{246,498},{386,506},{525,507},{661,501},{781,489},
  {120,333},{248,335},{381,336},{526,338},{662,336},{790,337},
  {124,178},{249,168},{382,162},{525,160},{663,166},{790,177}
};
// 定義預測格子內的藥丸信心閾值:
#define CLASS_CONFIDENCE 0.6f
// 定義偵測容許時間上限: (msec)
#define DETECT_TIME_THRESHOLD 10000 

// 定義目前偵測狀態: 0:未偵測, 1:偵測完畢
int detect_status=0; 

// 定義目前在偵測哪格子:
int cur_grid=0;

// class pillbox_manager{
// private:
//   std::vector<std::vector<ei_impulse_result_bounding_box_t>> grid;

// public:
//   // 紀錄資料
//   void record(std::vector<ei_impulse_result_bounding_box_t> &detected_pills, int grid_id){
//     if(grid_id>=18){
//       ei_printf("[pillbox_manager] Failed to put data in record!\n");
//       return;
//     }
    
//     grid[grid_id].push_back(detected_pills);
//   }

//   void print_grid(){
//     for(int i=0; i<grid.size(); i++){
//       ei_printf("");
//     }
//   }

// };


void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);
    //comment out the below line to start inference immediately after upload
    while (!Serial);
    Serial.println("Edge Impulse Inferencing Demo");
    if (ei_camera_init() == false) { ei_printf("Failed to initialize Camera!\r\n"); }
    else {
      ei_printf("Camera initialized\r\n");
    }
  // 初始化燈條並開啟燈條 start the  8+8 LEDs
  pixels.begin();
  Serial.println("pixles ready");
  pixels.clear();
  Serial.println("bottom+top");
  for(int i=0; i<16; i++){
    pixels.setPixelColor(i, pixels.Color(96,96,96));
    pixels.show();
    delay(DELAYVAL);
  }
  ei_printf("\nStarting continious inference in 2 seconds...\n");
  ei_sleep(2000);
}

String readString;
void loop()
{
  while(Serial.available()){
    char c = Serial.read();
    readString +=c;
    delay(5);
  }
  if(readString.length()>0){
    Serial.println(readString);
    int n=readString.toInt();
  
    if(n>=18){
      readString="";
      cur_grid=0;
    }else{
      cur_grid=n;
    }
    readString="";
  }
  // 累計偵測到的目標物數量:
  int target_count=0;

    // instead of wait_ms, we'll wait on the signal, this allows threads to cancel us...
    if (ei_sleep(5) != EI_IMPULSE_OK) { return; }
    snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);

    // check if allocation was successful
    if(snapshot_buf == nullptr) { ei_printf("ERR: Failed to allocate snapshot buffer!\n");
                                  return; }
    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;

    if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
        ei_printf("Failed to capture image\r\n");
        free(snapshot_buf);
        return;
    }
    // Run the classifier
    ei_impulse_result_t result = { 0 };

    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", err);
        return;
    }
    // print the predictions
    ei_printf("Predictions (DSP: %d ms., Classification: %d ms., count %d: \n",
                result.timing.dsp, result.timing.classification, result.bounding_boxes_count);

    ei_printf("Object detection bounding boxes:\r\n");
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        if(bb.value >= CLASS_CONFIDENCE){
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
        }
    }
    free(snapshot_buf);
}

/**
 * @brief   Setup image sensor & start streaming
 *
 * @retval  false if initialisation failed
 */
bool ei_camera_init(void) {

    if (is_initialised) return true;

    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }

    sensor_t * s = esp_camera_sensor_get();
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1); // flip it back
      s->set_brightness(s, 1); // up the brightness just a bit
      s->set_saturation(s, 0); // lower the saturation
    }
    // s->set_dcw(s, 0);


    is_initialised = true;
    return true;
}

/**
 * @brief      Stop streaming of sensor data
 */
void ei_camera_deinit(void) {

    //deinitialize the camera
    esp_err_t err = esp_camera_deinit();

    if (err != ESP_OK)
    {
        ei_printf("Camera deinit failed\n");
        return;
    }

    is_initialised = false;
    return;
}

/**
 * @brief      Capture, rescale and crop image
 *
 * @param[in]  img_width     width of output image
 * @param[in]  img_height    height of output image
 * @param[in]  out_buf       pointer to store output image, NULL may be used
 *                           if ei_camera_frame_buffer is to be used for capture and resize/cropping.
 *
 * @retval     false if not initialised, image captured, rescaled or cropped failed
 *
 */
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    bool do_resize = false;
    if (!is_initialised) { ei_printf("ERR: Camera is not initialized\r\n"); return false; }
    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) { ei_printf("Camera capture failed\n"); return false; }

    bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
    esp_camera_fb_return(fb);

    if(!converted){ ei_printf("Conversion failed\n"); return false; }
    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS) || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) { do_resize = true; }
    if (do_resize) {
      int res=-1;
      res=ei::image::processing::crop_image_rgb888_packed(
        out_buf, 
        EI_CAMERA_RAW_FRAME_BUFFER_COLS, 
        EI_CAMERA_RAW_FRAME_BUFFER_ROWS, 
        ROI[cur_grid].x, ROI[cur_grid].y, 
        out_buf, 
        EI_CLASSIFIER_INPUT_WIDTH,
        EI_CLASSIFIER_INPUT_HEIGHT);
      Serial.println(res);
        // 1(124,477)  7(120,333) 13(124,178)
        // 2(246,498)  8(248,335) 14(249,168)
        // 3(386,506)  9(381,336) 15(382,165)
        // 4(525,507) 10(526,338) 16(525,160)
        // 5(661,501) 11(662,336) 17(663,166)
        // 6(781,489) 12(790,337) 18(790,177)

        // ei::image::processing::crop_and_interpolate_rgb888(
        // out_buf,
        // EI_CAMERA_RAW_FRAME_BUFFER_COLS,
        // EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
        // out_buf,
        // img_width,
        // img_height);
    }
  return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    // we already have a RGB888 buffer, so recalculate offset into pixel index
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        // Swap BGR to RGB here
        // due to https://github.com/espressif/esp32-camera/issues/379
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];

        // go to the next pixel
        out_ptr_ix++;
        pixel_ix+=3;
        pixels_left--;
    }
    // and done!
    return 0;
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif
