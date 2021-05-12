/*#include "accelerometer_handler.h"
#include "config.h"
#include "magic_wand_model_data.h"
#include "mbed_rpc.h"

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

#include "mbed.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"
#include "stm32l475e_iot01_accelero.h"
#include <math.h>
#include "uLCD_4DGL.h"

DigitalOut myled(LED1);
DigitalOut myled2(LED2);
DigitalOut myled3(LED3);
InterruptIn Confirm_btn(USER_BUTTON);
BufferedSerial pc(USBTX, USBRX);
void MODE(Arguments *in, Reply *out);
RPCFunction rpcLED(&MODE, "MODE");
double x, y;
WiFiInterface *wifi;
volatile int message_num = 0;
volatile int arrivedcount = 0;
volatile bool closed = false;
Ticker flipper;
MQTT::Client<MQTTNetwork, Countdown>* client_out;
const char* topic = "Mbed";
Thread mqtt_thread(osPriorityHigh);
Thread t_gesture;
Thread t_tilt;
Thread t;
Config config;
//uLCD_4DGL uLCD(D1, D0, D2);

EventQueue mqtt_queue;
EventQueue queue;
int mode=0;
int Cmode=1;
// The gesture index of the prediction
int gesture_index;
// Create an area of memory to use for input, output, and intermediate arrays.
// The size of this will depend on the model you're using, and may need to be
// determined by experimentation.
constexpr int kTensorArenaSize = 60 * 1024;
uint8_t tensor_arena[kTensorArenaSize];
int situation=1;
int Go=0;
double angle=0.0;
int16_t old[3] = {0};


void select_feature(){

}

/////////////////////
// begin MQTT part //
/////////////////////
void messageArrived(MQTT::MessageData& md) {
    MQTT::Message &message = md.message;
    char msg[300];
    sprintf(msg, "Message arrived: QoS%d, retained %d, dup %d, packetID %d\r\n", message.qos, message.retained, message.dup, message.id);
    printf(msg);
    ThisThread::sleep_for(1000ms);
    char payload[300];
    sprintf(payload, "Payload %.*s\r\n", message.payloadlen, (char*)message.payload);
    printf(payload);
    ++arrivedcount;
}
void close_mqtt() {
    closed = true;
}

void Gesture(){
    char buff[200];

    message_num++;
    MQTT::Message message;
    if(gesture_index==0){
      sprintf(buff, "Gesture: circle");
    }else if(gesture_index==1){
      sprintf(buff, "Gesture: slope");
    }else if(gesture_index==2){
      sprintf(buff, "Gesture: line");
    }
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
    message.payload = (void*) buff;
    message.payloadlen = strlen(buff) + 1;
    int rc = client_out->publish(topic, message);

    printf("rc:  %d\r\n", rc);
    printf("Puslish message: %s\r\n", buff);

}

void decide_gesture() {

    wifi = WiFiInterface::get_default_instance();
    if (!wifi) {
            printf("ERROR: No WiFiInterface found.\r\n");
            //return -1;
    }


    printf("\nConnecting to %s...\r\n", MBED_CONF_APP_WIFI_SSID);
    int ret = wifi->connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD, NSAPI_SECURITY_WPA_WPA2);
    if (ret != 0) {
            printf("\nConnection error: %d\r\n", ret);
            //return -1;
    }
    NetworkInterface* net = wifi;
    MQTTNetwork mqttNetwork(net);
    MQTT::Client<MQTTNetwork, Countdown> client(mqttNetwork);
    client_out = &client;

    const char* host = "172.20.10.6";
    printf("Connecting to TCP network...\r\n");

    SocketAddress sockAddr;
    sockAddr.set_ip_address(host);
    sockAddr.set_port(1883);

    printf("address is %s/%d\r\n", (sockAddr.get_ip_address() ? sockAddr.get_ip_address() : "None"),  (sockAddr.get_port() ? sockAddr.get_port() : 0) ); //check setting

    int rc = mqttNetwork.connect(sockAddr);//(host, 1883);
    if (rc != 0) {
            printf("Connection error.");
            //return -1;
    }
    printf("Successfully connected!\r\n");

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    data.clientID.cstring = "Mbed";

    if ((rc = client.connect(data)) != 0){
            printf("Fail to connect MQTT\r\n");
    }
    if (client.subscribe(topic, MQTT::QOS0, messageArrived) != 0){
            printf("Fail to subscribe\r\n");
    }

    mqtt_thread.start(callback(&queue, &EventQueue::dispatch_forever));
    flipper.attach(queue.event(&Gesture),100ms);
    
    //btn3.rise(&close_mqtt);

    int num = 0;
    while (num != 5) {
            client.yield(100);
            ++num;
    }

    while (1) {
            if (closed) break;
            client.yield(500);
            ThisThread::sleep_for(500ms);
    }

    printf("Ready to close MQTT Network......\n");

    if ((rc = client.unsubscribe(topic)) != 0) {
            printf("Failed: rc from unsubscribe was %d\n", rc);
    }
    if ((rc = client.disconnect()) != 0) {
    printf("Failed: rc from disconnect was %d\n", rc);
    }

    mqttNetwork.disconnect();
    printf("Successfully closed!\n");

    //return 0;
}
///////////////////
// end MQTT part //
///////////////////


///////////////////
// begin ML part //
///////////////////
int PredictGesture(float* output) {
  // How many times the most recent gesture has been matched in a row
  static int continuous_count = 0;
  // The result of the last prediction
  static int last_predict = -1;

  // Find whichever output has a probability > 0.8 (they sum to 1)
  int this_predict = -1;
  for (int i = 0; i < label_num; i++) {
    if (output[i] > 0.8) this_predict = i;
  }

  // No gesture was detected above the threshold
  if (this_predict == -1) {
    continuous_count = 0;
    last_predict = label_num;
    return label_num;
  }

  if (last_predict == this_predict) {
    continuous_count += 1;
  } else {
    continuous_count = 0;
  }
  last_predict = this_predict;

  // If we haven't yet had enough consecutive matches for this gesture,
  // report a negative result
  if (continuous_count < config.consecutiveInferenceThresholds[this_predict]) {
    return label_num;
  }
  // Otherwise, we've seen a positive result, so clear all our variables
  // and report it
  continuous_count = 0;
  last_predict = -1;

  return this_predict;
}

void select_gesture() {
  t.start(decide_gesture);
  if(mode==1){
  // Whether we should clear the buffer next time we fetch data
  bool should_clear_buffer = false;
  bool got_data = false;

  // Set up logging.
  static tflite::MicroErrorReporter micro_error_reporter;
  tflite::ErrorReporter* error_reporter = &micro_error_reporter;

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  const tflite::Model* model = tflite::GetModel(g_magic_wand_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    error_reporter->Report(
        "Model provided is schema version %d not equal "
        "to supported version %d.",
        model->version(), TFLITE_SCHEMA_VERSION);
    //return -1;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  static tflite::MicroOpResolver<6> micro_op_resolver;
  micro_op_resolver.AddBuiltin(
      tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
      tflite::ops::micro::Register_DEPTHWISE_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_MAX_POOL_2D,
                               tflite::ops::micro::Register_MAX_POOL_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_CONV_2D,
                               tflite::ops::micro::Register_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_FULLY_CONNECTED,
                               tflite::ops::micro::Register_FULLY_CONNECTED());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_SOFTMAX,
                               tflite::ops::micro::Register_SOFTMAX());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_RESHAPE,
                               tflite::ops::micro::Register_RESHAPE(), 1);

  // Build an interpreter to run the model with
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
  tflite::MicroInterpreter* interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors
  interpreter->AllocateTensors();

  // Obtain pointer to the model's input tensor
  TfLiteTensor* model_input = interpreter->input(0);
  if ((model_input->dims->size != 4) || (model_input->dims->data[0] != 1) ||
      (model_input->dims->data[1] != config.seq_length) ||
      (model_input->dims->data[2] != kChannelNumber) ||
      (model_input->type != kTfLiteFloat32)) {
    error_reporter->Report("Bad input tensor parameters in model");
    //return -1;
  }

  int input_length = model_input->bytes / sizeof(float);

  TfLiteStatus setup_status = SetupAccelerometer(error_reporter);
  if (setup_status != kTfLiteOk) {
    error_reporter->Report("Set up failed\n");
    //return -1;
  }

  error_reporter->Report("Set up successful...\n");

  while (mode==1) {

    // Attempt to read new data from the accelerometer
    got_data = ReadAccelerometer(error_reporter, model_input->data.f,
                                 input_length, should_clear_buffer);

    // If there was no new data,
    // don't try to clear the buffer again and wait until next time
    if (!got_data) {
      should_clear_buffer = false;
      continue;
    }

    // Run inference, and report any error
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
      error_reporter->Report("Invoke failed on index: %d\n", begin_index);
      continue;
    }

    // Analyze the results to obtain a prediction
    gesture_index = PredictGesture(interpreter->output(0)->data.f);

    // Clear the buffer next time we read data
    should_clear_buffer = gesture_index < label_num;

    // Produce an output
    if (gesture_index < label_num) {
      error_reporter->Report(config.output_message[gesture_index]);
    }
  }
  }
  printf("finish1\n");
}
/////////////////////////////////////
// end ML part, decide the gesture //
/////////////////////////////////////

void MODE(Arguments *in, Reply *out){
    bool success = true;

    x = in->getArg<double>();

    // Have code here to call another RPC function to wake up specific led or close it.
    char buffer[200], outbuf[256];
    char strings[20];
    mode = x;
    int on = 1;

    sprintf(strings, "/mode%d/write %d", mode, on);
    strcpy(buffer, strings);
  
    t_gesture.start(select_gesture);
    t_tilt.start(select_feature);
    
    //uLCD_mode();
    

    if (success) {
        printf("success\n");
        out->putData(buffer);
        
    } else {
        out->putData("Failed to execute LED control.");
    }
    
}

int main(){

  config.seq_length=64;
  config.consecutiveInferenceThresholds[0]=20;
  config.consecutiveInferenceThresholds[1]=10;

  config.output_message[0]="circle";
  config.output_message[1]="slope";
  config.output_message[2]="line";

  char buf[256], outbuf[256]; 

  FILE *devin = fdopen(&pc, "r");
  FILE *devout = fdopen(&pc, "w");

  while(1) {
    memset(buf, 0, 256);      // clear buffer

    printf("Selection the mode:\n");
    printf("1.gesture mode\n");
    printf("Please follow the rule:/MODE/run (mode)\n");

    for(int i=0; ; i++) {
      char recv = fgetc(devin);
      if (recv == '\n') {
      printf("\r\n");
      break;
      }
      buf[i] = fputc(recv, devout);
    }
    //Call the static call method on the RPC class
    RPC::call(buf, outbuf);
    printf("%s\r\n", outbuf);
  }
}*/

#include "accelerometer_handler.h"
#include "config.h"
#include "magic_wand_model_data.h"
#include "mbed_rpc.h"

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

#include "mbed.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"
#include "stm32l475e_iot01_accelero.h"
#include <math.h>
#include "uLCD_4DGL.h"

DigitalOut myled(LED1);
DigitalOut myled2(LED2);
DigitalOut myled3(LED3);
InterruptIn Confirm_btn(USER_BUTTON);
BufferedSerial pc(USBTX, USBRX);
void MODE(Arguments *in, Reply *out);
RPCFunction rpcLED(&MODE, "MODE");
double x, y;
WiFiInterface *wifi;
volatile int message_num = 0;
volatile int arrivedcount = 0;
volatile bool closed = false;
Ticker flipper;
MQTT::Client<MQTTNetwork, Countdown>* client_out;
const char* topic = "Mbed";
Thread mqtt_thread(osPriorityHigh);
Thread t_gesture;
Thread t_tilt;
Config config;
uLCD_4DGL uLCD(D1, D0, D2);

EventQueue mqtt_queue;
EventQueue queue;
int mode=0;
int Cmode=1;
// The gesture index of the prediction
int gesture_index;
// Create an area of memory to use for input, output, and intermediate arrays.
// The size of this will depend on the model you're using, and may need to be
// determined by experimentation.
constexpr int kTensorArenaSize = 60 * 1024;
uint8_t tensor_arena[kTensorArenaSize];
int situation=1;
int Go=0;
double angle=0.0;
int16_t old[3] = {0};

/////////////////////
// begin MQTT part //
/////////////////////
void messageArrived(MQTT::MessageData& md) {
    MQTT::Message &message = md.message;
    char msg[300];
    sprintf(msg, "Message arrived: QoS%d, retained %d, dup %d, packetID %d\r\n", message.qos, message.retained, message.dup, message.id);
    printf(msg);
    ThisThread::sleep_for(1000ms);
    char payload[300];
    sprintf(payload, "Payload %.*s\r\n", message.payloadlen, (char*)message.payload);
    printf(payload);
    ++arrivedcount;
}
void close_mqtt() {
    closed = true;
}

void Angle(){
    BSP_ACCELERO_Init();

    double fix_z = 1000.0;
    int16_t pDataXYZ[3] = {0};
    char buff[200];
    BSP_ACCELERO_AccGetXYZ(pDataXYZ);
/*
    if(((pDataXYZ[2]-old[2]) < 4) && ((old[2]-pDataXYZ[2]) < 4)){
        myled3=1; // the sensor is still
        if(Go==0){
            Go = 1;
            fix_z = old[2];          
        }
    }else{
        myled3=0;
        old[0] = pDataXYZ[0];
        old[1] = pDataXYZ[1];
        old[2] = pDataXYZ[2];
    }
    double k;
    if(Go){
      double goal_angle=0.0;
      if(Cmode==1){
        goal_angle = 30.0;
      }else if(Cmode==2){
        goal_angle = 45.0;
      }
      else if(Cmode==3){
        goal_angle = 60.0;
      }
*/
      if(gesture_index!=3){  
        message_num++;
        MQTT::Message message;
        if(gesture_index==0){
          sprintf(buff, "circle");
          uLCD.cls();
          uLCD.text_width(1); 
          uLCD.text_height(1);
          uLCD.color(BLUE);
          uLCD.printf("\nCircle\n"); 
        }if(gesture_index==1){
          sprintf(buff, "slope");
          uLCD.cls();
          uLCD.text_width(1); 
          uLCD.text_height(1);
          uLCD.color(BLUE);
          uLCD.printf("\nSlope\n");           
        }else{
          sprintf(buff, "line");
          uLCD.cls();
          uLCD.text_width(1); 
          uLCD.text_height(1);
          uLCD.color(BLUE);
          uLCD.printf("\nLine\n"); 
        }
        
        message.qos = MQTT::QOS0;
        message.retained = false;
        message.dup = false;
        message.payload = (void*) buff;
        message.payloadlen = strlen(buff) + 1;
        int rc = client_out->publish(topic, message);

        printf("rc:  %d\r\n", rc);
        printf("Puslish message: %s\r\n", buff);
      }
}

void select_tilt() {
  if(mode==1){

    wifi = WiFiInterface::get_default_instance();
    if (!wifi) {
            printf("ERROR: No WiFiInterface found.\r\n");
            //return -1;
    }


    printf("\nConnecting to %s...\r\n", MBED_CONF_APP_WIFI_SSID);
    int ret = wifi->connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD, NSAPI_SECURITY_WPA_WPA2);
    if (ret != 0) {
            printf("\nConnection error: %d\r\n", ret);
            //return -1;
    }
    NetworkInterface* net = wifi;
    MQTTNetwork mqttNetwork(net);
    MQTT::Client<MQTTNetwork, Countdown> client(mqttNetwork);
    client_out = &client;

    const char* host = "172.20.10.6";
    printf("Connecting to TCP network...\r\n");

    SocketAddress sockAddr;
    sockAddr.set_ip_address(host);
    sockAddr.set_port(1883);

    printf("address is %s/%d\r\n", (sockAddr.get_ip_address() ? sockAddr.get_ip_address() : "None"),  (sockAddr.get_port() ? sockAddr.get_port() : 0) ); //check setting

    int rc = mqttNetwork.connect(sockAddr);//(host, 1883);
    if (rc != 0) {
            printf("Connection error.");
            //return -1;
    }
    printf("Successfully connected!\r\n");

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    data.clientID.cstring = "Mbed";

    if ((rc = client.connect(data)) != 0){
            printf("Fail to connect MQTT\r\n");
    }
    if (client.subscribe(topic, MQTT::QOS0, messageArrived) != 0){
            printf("Fail to subscribe\r\n");
    }

    mqtt_thread.start(callback(&queue, &EventQueue::dispatch_forever));
    flipper.attach(queue.event(&Angle),1s);
    
    //btn3.rise(&close_mqtt);

    int num = 0;
    while (num != 5) {
            client.yield(100);
            ++num;
    }

    while (1) {
            if (closed) break;
            client.yield(500);
            ThisThread::sleep_for(500ms);
    }

    printf("Ready to close MQTT Network......\n");

    if ((rc = client.unsubscribe(topic)) != 0) {
            printf("Failed: rc from unsubscribe was %d\n", rc);
    }
    if ((rc = client.disconnect()) != 0) {
    printf("Failed: rc from disconnect was %d\n", rc);
    }

    mqttNetwork.disconnect();
    printf("Successfully closed!\n");

    //return 0;
  }
}
///////////////////
// end MQTT part //
///////////////////
/*
/////////////////////////
// begin uLCD manifest //
/////////////////////////
void Mode_confirm(){
  Cmode=situation;
  uLCD.cls();
  mode=0;
  myled = 1;
  myled2 = 1;
  myled3 = 0;
  printf("%d\n",Cmode);
}
Thread t;
void uLCD_mode(){
  while(mode!=0){
    if(mode==1){
      situation1();
      while(mode==1){
        ThisThread::sleep_for(250ms);
        if (situation==1){     
          if (gesture_index==1){
              uLCD.cls();
              situation2();
              situation=2;
              gesture_index=3;
          }
        } else if (situation==2){         
          if (gesture_index==0){
            uLCD.cls();
            situation1();
            situation=1;
            gesture_index=3;
          } else if (gesture_index==1) {
            uLCD.cls();
            situation3();
            situation=3;
            gesture_index=3;
          }
        } else { 
          if (gesture_index==0){
              uLCD.cls();
              situation2();
              situation=2;
              gesture_index=3;
          }
        }
        t.start(callback(&queue, &EventQueue::dispatch_forever));
        Confirm_btn.rise(queue.event(Mode_confirm));
      }
    } else if(mode==2){
      uLCD.cls();
      show_angle();
    }
  }
}
///////////////////////
// end uLCD manifest //
///////////////////////
*/
///////////////////
// begin ML part //
///////////////////
int PredictGesture(float* output) {
  // How many times the most recent gesture has been matched in a row
  static int continuous_count = 0;
  // The result of the last prediction
  static int last_predict = -1;

  // Find whichever output has a probability > 0.8 (they sum to 1)
  int this_predict = -1;
  for (int i = 0; i < label_num; i++) {
    if (output[i] > 0.8) this_predict = i;
  }

  // No gesture was detected above the threshold
  if (this_predict == -1) {
    continuous_count = 0;
    last_predict = label_num;
    return label_num;
  }

  if (last_predict == this_predict) {
    continuous_count += 1;
  } else {
    continuous_count = 0;
  }
  last_predict = this_predict;

  // If we haven't yet had enough consecutive matches for this gesture,
  // report a negative result
  if (continuous_count < config.consecutiveInferenceThresholds[this_predict]) {
    return label_num;
  }
  // Otherwise, we've seen a positive result, so clear all our variables
  // and report it
  continuous_count = 0;
  last_predict = -1;

  return this_predict;
}

void select_gesture() {
  if(mode==1){
  // Whether we should clear the buffer next time we fetch data
  bool should_clear_buffer = false;
  bool got_data = false;

  // Set up logging.
  static tflite::MicroErrorReporter micro_error_reporter;
  tflite::ErrorReporter* error_reporter = &micro_error_reporter;

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  const tflite::Model* model = tflite::GetModel(g_magic_wand_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    error_reporter->Report(
        "Model provided is schema version %d not equal "
        "to supported version %d.",
        model->version(), TFLITE_SCHEMA_VERSION);
    //return -1;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  static tflite::MicroOpResolver<6> micro_op_resolver;
  micro_op_resolver.AddBuiltin(
      tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
      tflite::ops::micro::Register_DEPTHWISE_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_MAX_POOL_2D,
                               tflite::ops::micro::Register_MAX_POOL_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_CONV_2D,
                               tflite::ops::micro::Register_CONV_2D());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_FULLY_CONNECTED,
                               tflite::ops::micro::Register_FULLY_CONNECTED());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_SOFTMAX,
                               tflite::ops::micro::Register_SOFTMAX());
  micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_RESHAPE,
                               tflite::ops::micro::Register_RESHAPE(), 1);

  // Build an interpreter to run the model with
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
  tflite::MicroInterpreter* interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors
  interpreter->AllocateTensors();

  // Obtain pointer to the model's input tensor
  TfLiteTensor* model_input = interpreter->input(0);
  if ((model_input->dims->size != 4) || (model_input->dims->data[0] != 1) ||
      (model_input->dims->data[1] != config.seq_length) ||
      (model_input->dims->data[2] != kChannelNumber) ||
      (model_input->type != kTfLiteFloat32)) {
    error_reporter->Report("Bad input tensor parameters in model");
    //return -1;
  }

  int input_length = model_input->bytes / sizeof(float);

  TfLiteStatus setup_status = SetupAccelerometer(error_reporter);
  if (setup_status != kTfLiteOk) {
    error_reporter->Report("Set up failed\n");
    //return -1;
  }

  error_reporter->Report("Set up successful...\n");

  while (mode==1) {

    // Attempt to read new data from the accelerometer
    got_data = ReadAccelerometer(error_reporter, model_input->data.f,
                                 input_length, should_clear_buffer);

    // If there was no new data,
    // don't try to clear the buffer again and wait until next time
    if (!got_data) {
      should_clear_buffer = false;
      continue;
    }

    // Run inference, and report any error
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
      error_reporter->Report("Invoke failed on index: %d\n", begin_index);
      continue;
    }

    // Analyze the results to obtain a prediction
    gesture_index = PredictGesture(interpreter->output(0)->data.f);

    // Clear the buffer next time we read data
    should_clear_buffer = gesture_index < label_num;

    // Produce an output
    if (gesture_index < label_num) {
      error_reporter->Report(config.output_message[gesture_index]);
      //if(gesture_index==0) G_char="C";
      //else(gesture_index==0) 
    }
  }
  }
  printf("finish1\n");
}
/////////////////////////////////////
// end ML part, decide the gesture //
/////////////////////////////////////
int edge;
void feature(){
  BSP_ACCELERO_Init();
  int16_t pDataXYZ[3] = {0};
  int16_t old[3] = {0};
  BSP_ACCELERO_AccGetXYZ(pDataXYZ);
  int line_h=0;
  int line_v=0;
  edge=0;
  while(1){
  for(int x=0; x<4; x++){ //calculate straight line

    if((old[0]-pDataXYZ[0]<4) && (pDataXYZ[0]-old[0]<4)) line_h=1;
    if((old[1]-pDataXYZ[1]<4) && (pDataXYZ[1]-old[1]<4)) line_v=1;

  }
    if(line_h && line_v) edge=2;
    if(line_h && !line_v) edge=1;
    if(line_v && !line_h) edge=1;
    old[0]=pDataXYZ[0];
    old[1]=pDataXYZ[1];
    old[2]=pDataXYZ[2];
  }
}

void MODE(Arguments *in, Reply *out){
    bool success = true;

    x = in->getArg<double>();

    // Have code here to call another RPC function to wake up specific led or close it.
    char buffer[200], outbuf[256];
    char strings[20];
    mode = x;
    int on = 1;

    sprintf(strings, "/mode%d/write %d", mode, on);
    strcpy(buffer, strings);
    
    if(mode==1){
      t_gesture.start(select_gesture);
      t_tilt.start(select_tilt);
    }
    feature();
    //uLCD_mode();
    

    if (success) {
        printf("success\n");
        out->putData(buffer);
        
    } else {
        out->putData("Failed to execute LED control.");
    }
    
}

int main(){

  config.seq_length=64;
  config.consecutiveInferenceThresholds[0]=20;
  config.consecutiveInferenceThresholds[1]=10;
  config.output_message[0]="circle";
  config.output_message[1]="slope";
  config.output_message[2]="line";

  char buf[256], outbuf[256]; 

  FILE *devin = fdopen(&pc, "r");
  FILE *devout = fdopen(&pc, "w");

  while(1) {
    memset(buf, 0, 256);      // clear buffer

    printf("Selection the mode:\n");
    printf("1.gesture mode\n");
    printf("Please follow the rule:/MODE/run (mode)\n");

    for(int i=0; ; i++) {
      char recv = fgetc(devin);
      if (recv == '\n') {
      printf("\r\n");
      break;
      }
      buf[i] = fputc(recv, devout);
    }
    //Call the static call method on the RPC class
    RPC::call(buf, outbuf);
    printf("%d",edge);
    printf("%s\r\n", outbuf);
  }
}