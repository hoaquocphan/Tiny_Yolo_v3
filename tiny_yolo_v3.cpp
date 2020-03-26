#include <assert.h>
#include <onnxruntime_c_api.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>
#include <map>
#include <fstream>
#include <sys/time.h>
#include <iostream>
#include <numeric>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/video/background_segm.hpp"
#include "opencv2/video/tracking.hpp"
/*****************************************
* Includes
******************************************/
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <jpeglib.h>
#include <termios.h>
#include <math.h>
#include <iomanip>
#include <sstream>
//#include <CommandAllocatorRing.h>

#include "box.h"
#include "define.h"
#include "image.h"

using namespace cv;
using namespace std;
/*****************************************
* Global Variables
******************************************/
std::map<int,std::string> label_file_map;
char inference_mode = DETECTION;
int model=0;
char* save_filename = "output.jpg";
const char* input_file = "yolo004.jpg";
const char* mat_out = "mat_out.jpg";

/*
double anchors[] = {
    1.08,   1.19,
    3.42,   4.41,
    6.63,   11.38,
    9.42,   5.11,
    16.62,  10.52
};
*/
/*
double anchors[] = { //for tinyYolov3
    10,   14,
    23,   27,
    37,   58,
    81,   82,
    135,  169,
    344,  319
};
*/
// tinyyolov3: 10,14,  23,27,  37,58,  81,82,  135,169,  344,319
// yolov3 10,13,  16,30,  33,23,  30,61,  62,45,  59,119,  116,90,  156,198,  373,326

double anchors[] = { //for Yolov3
    116,90,
    156,198,
    373,326,
    30,61,
    62,45,
    59,119,
    10,13,
    16,30,
    33,23
};

const OrtApi* g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);


void CheckStatus(OrtStatus* status)
{
    if (status != NULL) {
        const char* msg = g_ort->GetErrorMessage(status);
        fprintf(stderr, "%s\n", msg);
        g_ort->ReleaseStatus(status);
        exit(1);
    }
}


/*****************************************
* Function Name :  loadLabelFile
* Description       : Load txt file
* Arguments         :
* Return value  :
******************************************/
int loadLabelFile(std::string label_file_name)
{
    int counter = 0;
    std::ifstream infile(label_file_name);

    if (!infile.is_open())
    {
        perror("error while opening file");
        return -1;
    }

    std::string line;
    while(std::getline(infile,line))
    {
        label_file_map[counter++] = line;
    }

    if (infile.bad())
    {
        perror("error while reading file");
        return -1;
    }

    return 0;
}

/*****************************************
* Function Name : sigmoid
* Description   : helper function for YOLO Post Processing
* Arguments :
* Return value  :
******************************************/
double sigmoid(double x){
    return 1.0/(1.0+exp(-x));
}

/*****************************************
* Function Name : softmax
* Description   : helper function for YOLO Post Processing
* Arguments :
* Return value  :
******************************************/
void softmax(float val[]){
    float max = -INT_MAX;
    float sum = 0;

    for (int i = 0;i<80;i++){
        max = std::max(max, val[i]);
    }

    for (int i = 0;i<80;i++){
        val[i]= (float) exp(val[i]-max);
        sum+= val[i];
    }

    for (int i = 0;i<80;i++){
        val[i]= val[i]/sum;
    }

    // printf("Softmax: max %f sum %f\n", max, sum);
}

/*****************************************
* Function Name : timedifference_msec
* Description   :
* Arguments :
* Return value  :
******************************************/
static double timedifference_msec(struct timeval t0, struct timeval t1)
{
    return (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
}


/*****************************************
* Function Name :offset
* Description   : c
* Arguments :
* Return value  :
******************************************/
int offset(int o,int channel){
    return  o+ channel*YOLO_GRID_X*YOLO_GRID_Y;
}

/*****************************************
* Function Name :offset
* Description       : c
* Arguments         :
* Return value  :
******************************************/
int offset_(int b, int y, int x){
    return b*(20+5)* YOLO_GRID_X * YOLO_GRID_Y + y * YOLO_GRID_X + x;
}

/*****************************************
* Function Name : print_box
* Description   : Function to printout details of single bounding box to standard output
* Arguments :   detection d: detected box details
*             int i : Result number
* Return value  :
******************************************/
void print_box(detection d, int i){
    printf("\x1b[4m"); //Change colour
    printf("\nResult %d\n", i);
    printf("\x1b[0m"); //Change the top first colour
    printf("\x1b[1m"); //Change the top first colour
    printf("Detected        : %s\n",label_file_map[d.c].c_str());//, detected
    printf("\x1b[0m"); //Change the colour to default
    printf("Bounding Box    : (X, Y, W, H) = (%.2f, %.2f, %.2f, %.2f)\n", d.bbox.x, d.bbox.y, d.bbox.w, d.bbox.h);
    printf("Confidence (IoU): %.1f %%\n", d.conf*100);
    printf("Probability     : %.1f %%\n",  d.prob*100);
    printf("Score           : %.1f %%\n", d.prob * d.conf*100);
}

int main(int argc, char* argv[])
{
    //Config : inference mode
    inference_mode = DETECTION;
    //Config : model
    std::string model_name = "yolov3-tiny.onnx";
    //std::string model_path= "yolov3-tiny/yolov3-tiny.onnx";
    std::string model_path= "yolov3-tiny/yolov3.onnx";
    
    printf("Start Loading Model %s\n", model_name.c_str());

    int img_sizex, img_sizey, img_channels;

    //Postprocessing Variables
    float th_conf = 0.5;
    float th_prob = 0.5;
    int count = 0;
    std::vector<detection> det;

    //Timing Variables
    struct timeval start_time, stop_time;
    double diff, diff_capture;

    //UNCOMMENT to use dog image as an input

    stbi_uc * img_data = stbi_load(input_file, &img_sizex, &img_sizey, &img_channels, STBI_default);

    std::vector<float> input_tensor_values_shape(2);
    input_tensor_values_shape[0] = img_sizey;
    input_tensor_values_shape[1] = img_sizex;

    /////////////
    int sizex=416, sizey=416;
    int img_sizex_new,img_sizey_new;
    double scale;
    scale = ((double)sizex/(double)img_sizex) < ((double)sizey/(double)img_sizey) ? ((double)sizex/(double)img_sizex) : ((double)sizey/(double)img_sizey);
    img_sizex_new = (int)(scale * img_sizex);
    img_sizey_new = (int)(scale * img_sizey);
    printf("img_sizex: %d\n",img_sizex);
    printf("img_sizey: %d\n",img_sizey);
    printf("scale: %f\n",scale);
    printf("img_sizex_new: %d\n",img_sizex_new);
    printf("img_sizey_new: %d\n",img_sizey_new);
    cv::Mat img = cv::imread(input_file, cv::IMREAD_COLOR);  // (720, 960, 3)
    cv::Mat img_resize;
    cv::resize(img, img_resize, cv::Size(img_sizex_new,img_sizey_new));
    cv::Mat new_image(sizex,sizey, CV_8UC3, Scalar(128,128,128));

    img_resize.copyTo(new_image(cv::Rect(0,0,img_resize.cols, img_resize.rows)));
    cv::imwrite(mat_out, new_image);
    printf("cols: %d\n",new_image.cols);
    printf("rows: %d\n",new_image.rows);
    stbi_uc * img_data_new = stbi_load(mat_out, &img_sizex, &img_sizey, &img_channels, STBI_default);
    //////////////


    struct S_Pixel
    {
        unsigned char RGBA[3];
    };
    const S_Pixel * imgPixels(reinterpret_cast<const S_Pixel *>(img_data_new));

    //Config: label txt
    std::string filename("coco_classes.txt");

    //ONNX runtime: Necessary
    OrtEnv* env;
    CheckStatus(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "test", &env));

    //ONNX runtime: Necessary
    OrtSession* session;
    OrtSessionOptions* session_options;
    g_ort->CreateSessionOptions(&session_options);
    g_ort->SetInterOpNumThreads(session_options, 2); //Multi-core
    CheckStatus(g_ort->CreateSession(env, model_path.c_str(), session_options, &session));

    size_t num_input_nodes;
    size_t num_output_nodes;
    OrtStatus* status;
    OrtAllocator* allocator;
    CheckStatus(g_ort->GetAllocatorWithDefaultOptions(&allocator));

    status = g_ort->SessionGetInputCount(session, &num_input_nodes);
    status = g_ort->SessionGetOutputCount(session, &num_output_nodes);

    std::vector<const char*> input_node_names(num_input_nodes);
    std::vector<const char*> output_node_names(num_output_nodes);
    std::vector<int64_t> input_node_dims_input;
    std::vector<int64_t> input_node_dims_shape;
    std::vector<int64_t> output_node_dims;
    printf("\nCurrent Model is %s\n",model_name.c_str());
    printf("Number of inputs = %zu\n", num_input_nodes);
    printf("Number of outputs = %zu\n", num_output_nodes);

    
    for (size_t i = 0; i < num_output_nodes; i++) {
        ////    // print input node names
        char* output_name;
        status = g_ort->SessionGetOutputName(session, i, allocator, &output_name);
        printf("output %d : name=%s\n", i, output_name);
        output_node_names[i] = output_name;
     ////// print input node types
        OrtTypeInfo* typeinfo;
        status = g_ort->SessionGetOutputTypeInfo(session, i, &typeinfo);
        const OrtTensorTypeAndShapeInfo* tensor_info;
        CheckStatus(g_ort->CastTypeInfoToTensorInfo(typeinfo,&tensor_info));
        ONNXTensorElementDataType type;
        CheckStatus(g_ort->GetTensorElementType(tensor_info,&type));
        printf("Output %d : type=%d\n", i, type);
        //// print input shapes/dims
        size_t num_dims = 3;
        printf("Output %d : num_dims=%zu\n", i, num_dims);
        output_node_dims.resize(num_dims);
        g_ort->GetDimensions(tensor_info, (int64_t*)output_node_dims.data(), num_dims);
        
        size_t count;
        g_ort->GetTensorShapeElementCount(tensor_info, &count);
        printf("count %d \n", count);
        
        for (int j = 0; j < num_dims; j++) {
            printf("Output %d : dim %d=%jd\n", i, j, output_node_dims[j]);
        }
    
        g_ort->ReleaseTypeInfo(typeinfo);
    }



// Print Out Input details
    // iterate over all input nodes
    for (size_t i = 0; i < num_input_nodes; i++){
        // print input node names
        char* input_name;
        status = g_ort->SessionGetInputName(session, i, allocator, &input_name);
        printf("Input %zu : name=%s\n", i, input_name);
        input_node_names[i] = input_name;

        // print input node types
        OrtTypeInfo* typeinfo;
        status = g_ort->SessionGetInputTypeInfo(session, i, &typeinfo);
        const OrtTensorTypeAndShapeInfo* tensor_info;
        CheckStatus(g_ort->CastTypeInfoToTensorInfo(typeinfo,&tensor_info));
        ONNXTensorElementDataType type;
        CheckStatus(g_ort->GetTensorElementType(tensor_info,&type));
        printf("Input %zu : type=%d\n", i, type);
        size_t num_dims;
        if(i == 0)
        {
            num_dims = 4;
            printf("Input %zu : num_dims=%zu\n", i, num_dims);
            input_node_dims_input.resize(num_dims);
            g_ort->GetDimensions(tensor_info, (int64_t*)input_node_dims_input.data(), num_dims);
            if(input_node_dims_input[0]<0){//Necessary for  Tiny YOLO v3
                input_node_dims_input[0]=1;   //Change the first dimension from -1 to 1
            }
            if(input_node_dims_input[2]<0){//Necessary for  Tiny YOLO v3
                input_node_dims_input[2]=416;   //Change the first dimension from -1 to 416
            }
            if(input_node_dims_input[3]<0){//Necessary for  Tiny YOLO v3
                input_node_dims_input[3]=416;   //Change the first dimension from -1 to 416
            }
            for (size_t j = 0; j < num_dims; j++) printf("Input %zu : dim %zu=%jd\n", i, j, input_node_dims_input[j]);
        }
        else if(i == 1)
        {
            num_dims = 2;
            printf("Input %zu : num_dims=%zu\n", i, num_dims);
            input_node_dims_shape.resize(num_dims);
            g_ort->GetDimensions(tensor_info, (int64_t*)input_node_dims_shape.data(), num_dims);
            if(input_node_dims_shape[0]<0){//Necessary for  Tiny YOLO v3
                input_node_dims_shape[0]=1;   //Change the first dimension from -1 to 1
            }
            for (size_t j = 0; j < num_dims; j++) printf("Input %zu : dim %zu=%jd\n", i, j, input_node_dims_shape[j]);
        } 

        g_ort->ReleaseTypeInfo(typeinfo);
    }


    //g_ort->ReleaseMemory(allocator);
    //ONNX: Prepare input container
    size_t input_tensor_size = img_sizex * img_sizey * 3;
    std::vector<float> input_tensor_values(input_tensor_size);
    std::vector<float> input_tensor_values_new(input_tensor_size);
    int frame_count = 0;
    size_t offs, c, y, x;
    std::map<float,int> result; //Output for classification

    Image *imge = new Image(img_sizex, img_sizey, 3);
    //Transpose
    offs = 0;
    for ( c = 0; c < 3; c++){
        for ( y = 0; y < img_sizey; y++){
            for ( x = 0; x < img_sizex; x++, offs++){
                const int val(imgPixels[y * img_sizex + x].RGBA[c]);
                imge->set((y*img_sizex+x)*3+c, val);
                //if(offs < 40){printf("val %d: %d\n",offs,val);}
                //printf("val2: %.6f\n",(float)(imge->img_buffer[(y*img_sizex+x)*3+c])/255);
                input_tensor_values[offs] = ((float)val)/255;
            }
        }
    }
    

    // create input tensor object from data values
    OrtMemoryInfo* memory_info;
    CheckStatus(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_info));

    
    std::vector< std::vector<int64_t> > input_node_dims;
    input_node_dims.push_back(input_node_dims_input);
    input_node_dims.push_back(input_node_dims_shape);
    
    
    //std::vector<OrtValue* > input_tensor(input_node_names.size());
    OrtValue* input_tensor_image = NULL;
    OrtValue* input_tensor_shape = NULL;
    //OrtValue* input_tensor[2] = {nullptr};
    
    CheckStatus(g_ort->CreateTensorWithDataAsOrtValue(memory_info, input_tensor_values.data(), input_tensor_size*sizeof(float), input_node_dims_input.data(), 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor_image));
    CheckStatus(g_ort->CreateTensorWithDataAsOrtValue(memory_info, input_tensor_values_shape.data(), 2*sizeof(float), input_node_dims_shape.data(), 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor_shape));
    //CheckStatus(g_ort->CreateTensorWithDataAsOrtValue(memory_info, input_tensor_values.data(), input_tensor_size*sizeof(float), input_node_dims_input.data(), 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor[0]));
    //CheckStatus(g_ort->CreateTensorWithDataAsOrtValue(memory_info, input_tensor_values_shape.data(), 2*sizeof(float), input_node_dims_shape.data(), 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_tensor[1]));

    
    int is_tensor;
    CheckStatus(g_ort->IsTensor(input_tensor_image,&is_tensor));
    assert(is_tensor);
    CheckStatus(g_ort->IsTensor(input_tensor_shape,&is_tensor));
    assert(is_tensor);
    g_ort->ReleaseMemoryInfo(memory_info);
    
    
    
    std::vector<const OrtValue*> input_tensor(2);
    //input_tensor.push_back((const OrtValue* )input_tensor_image);
    //input_tensor.push_back((const OrtValue* )input_tensor_shape);
    
    
    input_tensor[0] = input_tensor_image;
    input_tensor[1] = input_tensor_shape;
    
    CheckStatus(g_ort->IsTensor(input_tensor[0],&is_tensor));
    assert(is_tensor);
    
    printf("input_tensor_image %zu \n", input_tensor_image);
    printf("input_tensor_shape %zu \n", input_tensor_shape);
    printf("input_tensor %zu \n", input_tensor.data());
    printf("input_tensor[0] %zu \n", input_tensor[0]);
    printf("input_tensor[1] %zu \n", input_tensor[1]);
    
    
    // RUN: score model & input tensor, get back output tensor
    //OrtValue* output_tensor[3] = {nullptr};
    std::vector<OrtValue *> output_tensor(3);
    OrtValue* output_tensor_boxes = NULL;
    OrtValue* output_tensor_scores = NULL;
    OrtValue* output_tensor_classes = NULL;
    //output_tensor[0] = output_tensor_boxes;
    //output_tensor[1] = output_tensor_scores;
    //output_tensor[2] = output_tensor_classes;
    
    //std::vector<const char*> input_names = { "image_shape", "input_1"};
    //std::vector<const char*> input_names = { "input_1", "image_shape"};
    
    gettimeofday(&start_time, nullptr);
    CheckStatus(g_ort->Run(session, NULL, input_node_names.data(), input_tensor.data(), 2, output_node_names.data(), 3, output_tensor.data()));
    //CheckStatus(g_ort->Run(session, NULL, input_names.data(), &input_tensor[0], 2, output_node_names.data(), 3, output_tensor));
    //CheckStatus(g_ort->Run(session, NULL, input_names.data(), input_tensor.data(), 2, output_node_names.data(), 3, output_tensor.data()));
    
    gettimeofday(&stop_time, nullptr);
    
    output_tensor_boxes = output_tensor[0];
    output_tensor_scores = output_tensor[1];
    output_tensor_classes = output_tensor[2];
    
    CheckStatus(g_ort->IsTensor(output_tensor_boxes,&is_tensor));
    assert(is_tensor);
    CheckStatus(g_ort->IsTensor(output_tensor_scores,&is_tensor));
    assert(is_tensor);
    CheckStatus(g_ort->IsTensor(output_tensor_classes,&is_tensor));
    assert(is_tensor);
    
    diff = timedifference_msec(start_time,stop_time);

    
    // Get pointer to output tensor float values
    float* out1 = NULL;
    g_ort->GetTensorMutableData(output_tensor_boxes, (void**)&out1);
    for(size_t i = 0; i<10; i++){
        printf("out1: %d", i);
        printf(" output: %f\n", out1[i]);
    }
    float* out2 = NULL;
    g_ort->GetTensorMutableData(output_tensor_scores, (void**)&out2);
    for(size_t i = 0; i<10; i++){
        printf("out2: %d", i);
        printf(" output: %f\n", out2[i]);
    }
    float* out3 = NULL;
    g_ort->GetTensorMutableData(output_tensor_classes, (void**)&out3);
    for(size_t i = 0; i<10; i++){
        printf("out3: %d", i);
        printf(" output: %f\n", out3[i]);
    }
    
    
    if(loadLabelFile(filename) != 0)
    {
        fprintf(stderr,"Fail to open or process file %s\n",filename.c_str());
        delete imge;
        return -1;
    }
    
    int grid_h1=13, grid_w1=13, grid_h2=26, grid_w2=26, grid_h3=52, grid_w3=52, nb_box = 3;
    int nb_class = label_file_map.size();
    
    
    //[(1, 13, 13, 255), (1, 26, 26, 255), (1, 52, 52, 255)]
    
    float output1[13][13][3][85];
    float output2[26][26][3][85];
    float output3[52][52][3][85];
    /*
    offs = 0; 
    for(size_t x = 0; x < 13; x++)
    {
        for(size_t y = 0; y < 13; y++)
        {
            for(size_t z = 0; z < 3; z++)
            {
                for(size_t t = 0; t < 85; t++,offs++)
                {
                    output1[x][y][z][t] = out1[offs];
                }
            }
        }
    }
    offs = 0; 
    for(size_t x = 0; x < 26; x++)
    {
        for(size_t y = 0; y < 26; y++)
        {
            for(size_t z = 0; z < 3; z++)
            {
                for(size_t t = 0; t < 85; t++,offs++)
                {
                    output2[x][y][z][t] = out2[offs];
                }
            }
        }
    }
    offs = 0; 
    for(size_t x = 0; x < 52; x++)
    {
        for(size_t y = 0; y < 52; y++)
        {
            for(size_t z = 0; z < 3; z++)
            {
                for(size_t t = 0; t < 85; t++,offs++)
                {
                    output3[x][y][z][t] = out3[offs];
                }
            }
        }
    }
    
    
    
    
    for(size_t x = 0; x < grid_w1; x++)
    {
        for(size_t y = 0; y < grid_h1; y++)
        {
            for(size_t z = 0; z < nb_box; z++)
            {
                for(size_t t = 0; t < 2; t++)
                {
                    output1[x][y][z][t] = sigmoid(output1[x][y][z][t]);
                }
                for(size_t t = 4; t < 85; t++)
                {
                    output1[x][y][z][t] = sigmoid(output1[x][y][z][t]);
                }
                for(size_t t = 5; t < 85; t++)
                {
                    output1[x][y][z][t] = output1[x][y][z][4] * output1[x][y][z][t];
                    if(output1[x][y][z][t] <= th_conf)
                    {
                        output1[x][y][z][t] = 0;
                    }
                }
            }
        }
    }
    
    for(size_t x = 0; x < grid_w2; x++)
    {
        for(size_t y = 0; y < grid_h2; y++)
        {
            for(size_t z = 0; z < nb_box; z++)
            {
                for(size_t t = 0; t < 2; t++)
                {
                    output2[x][y][z][t] = sigmoid(output2[x][y][z][t]);
                }
                for(size_t t = 4; t < 85; t++)
                {
                    output2[x][y][z][t] = sigmoid(output2[x][y][z][t]);
                }
                for(size_t t = 5; t < 85; t++)
                {
                    output2[x][y][z][t] = output2[x][y][z][4] * output2[x][y][z][t];
                    if(output2[x][y][z][t] <= th_conf)
                    {
                        output2[x][y][z][t] = 0;
                    }
                }
            }
        }
    }
    
    for(size_t x = 0; x < grid_w3; x++)
    {
        for(size_t y = 0; y < grid_h3; y++)
        {
            for(size_t z = 0; z < nb_box; z++)
            {
                for(size_t t = 0; t < 2; t++)
                {
                    output3[x][y][z][t] = sigmoid(output3[x][y][z][t]);
                }
                for(size_t t = 4; t < 85; t++)
                {
                    output3[x][y][z][t] = sigmoid(output3[x][y][z][t]);
                }
                for(size_t t = 5; t < 85; t++)
                {
                    output3[x][y][z][t] = output3[x][y][z][4] * output3[x][y][z][t];
                    if(output3[x][y][z][t] <= th_conf)
                    {
                        output3[x][y][z][t] = 0;
                    }
                }
            }
        }
    }
    */
    
    float temp1[13*13*3*85];
    float temp2[26*26*3*85];
    float temp3[52*52*3*85];
    char* data_file1 = "data_13x13.txt";
    char* data_file2 = "data_26x26.txt";
    char* data_file3 = "data_52x52.txt";
    int n = 0;
    
    FILE *fp;
    fp = fopen(data_file1, "r");
    while (fscanf(fp, "%f", &temp1[n++]) != EOF){}
    n=0;
    for(size_t x = 0; x < 13; x++)
    {
        for(size_t y = 0; y < 13; y++)
        {
            for(size_t z = 0; z < 3; z++)
            {
                for(size_t t = 0; t < 85; t++,n++)
                {
                    output1[x][y][z][t] = temp1[n];
                }
                for(size_t t = 0; t < 2; t++)
                {
                    output1[x][y][z][t] = sigmoid(output1[x][y][z][t]);
                }
                for(size_t t = 4; t < 85; t++)
                {
                    output1[x][y][z][t] = sigmoid(output1[x][y][z][t]);
                }
                for(size_t t = 5; t < 85; t++)
                {
                    output1[x][y][z][t] = output1[x][y][z][4] * output1[x][y][z][t];
                    if(output1[x][y][z][t] <= th_conf)
                    {
                        output1[x][y][z][t] = 0;
                    }
                }
            }
        }
    }
    fclose(fp);
    n=0;
    fp = fopen(data_file2, "r");
    while (fscanf(fp, "%f", &temp2[n++]) != EOF){}
    n=0;
    for(size_t x = 0; x < 26; x++)
    {
        for(size_t y = 0; y < 26; y++)
        {
            for(size_t z = 0; z < 3; z++)
            {
                for(size_t t = 0; t < 85; t++,n++)
                {
                    output2[x][y][z][t] = temp2[n];
                }
                for(size_t t = 0; t < 2; t++)
                {
                    output2[x][y][z][t] = sigmoid(output2[x][y][z][t]);
                }
                for(size_t t = 4; t < 85; t++)
                {
                    output2[x][y][z][t] = sigmoid(output2[x][y][z][t]);
                }
                for(size_t t = 5; t < 85; t++)
                {
                    output2[x][y][z][t] = output2[x][y][z][4] * output2[x][y][z][t];
                    if(output2[x][y][z][t] <= th_conf)
                    {
                        output2[x][y][z][t] = 0;
                    }
                }
            }
        }
    }
    fclose(fp);
    n=0;
    fp = fopen(data_file3, "r");
    while (fscanf(fp, "%f", &temp3[n++]) != EOF){}
    n=0;
    for(size_t x = 0; x < 52; x++)
    {
        for(size_t y = 0; y < 52; y++)
        {
            for(size_t z = 0; z < 3; z++)
            {
                for(size_t t = 0; t < 85; t++,n++)
                {
                    output3[x][y][z][t] = temp3[n];
                }
                //for(size_t t = 0; t < 2; t++)
                for(size_t t = 0; t < 4; t++)
                {
                    output3[x][y][z][t] = sigmoid(output3[x][y][z][t]);
                }
                for(size_t t = 4; t < 85; t++)
                {
                    output3[x][y][z][t] = sigmoid(output3[x][y][z][t]);
                }
                for(size_t t = 5; t < 85; t++)
                {
                    output3[x][y][z][t] = output3[x][y][z][4] * output3[x][y][z][t];
                    if(output3[x][y][z][t] <= th_conf)
                    {
                        output3[x][y][z][t] = 0;
                    }
                }
            }
        }
    }
    fclose(fp);
    
    
    for(int i = 0; i < grid_w1*grid_h1; i++)
    {
        float row = (float)i / (float)grid_w1;
        float col = i % grid_w1;
        for(int b = 0; b < nb_box; b++)
        {
            float objectness = output1[int(row)][int(col)][b][4];
            if(objectness > th_conf)
            {
                float x, y, w, h;
                x = output1[int(row)][int(col)][b][0];
                y = output1[int(row)][int(col)][b][1];
                w = output1[int(row)][int(col)][b][2];
                h = output1[int(row)][int(col)][b][3];
                
                float xPos = (col + x)*32;
                float yPos = (row + y)*32;/
                float wBox = anchors[2 * b + 0] * exp(w);
                float hBox = anchors[2 * b + 1] * exp(h);
                
                
                Box bb = float_to_box(xPos, yPos, wBox, hBox);
                
                float classes[80];
                for (int c = 0;c<80;c++){
                    classes[c] = output1[int(row)][int(col)][b][c+5];
                }
                float max_pd = 0;
                int detected = -1;
                for (int c = 0;c<80;c++){
                    if (classes[c]>max_pd){
                        detected = c;
                        max_pd = classes[c];
                    }
                }
                float score = max_pd;
                if (score>th_prob){
                    detection d = { bb, objectness , detected,max_pd };
                    det.push_back(d);
                    count++;
                }
            }
        }
    }
    
    //correct_yolo_boxes
    filter_boxes_nms(det, count, 0.6);
    
    int i, j=0;
    //Render boxes on image and print their details
    for(i =0;i<count;i++){
        if(det[i].prob == 0) continue;
        j++;
        print_box(det[i], j);
        std::stringstream stream;
        stream << std::fixed << std::setprecision(2) << det[i].conf*det[i].prob;
        std::string result_str = label_file_map[det[i].c]+ " "+ stream.str();
        imge->drawRect((int)det[i].bbox.x, (int)det[i].bbox.y, (int)det[i].bbox.w, (int)det[i].bbox.h, (int)det[i].c, result_str.c_str());
    }
    gettimeofday(&stop_time, nullptr);//Stop postproc timer
    size_t time_post = timedifference_msec(start_time,stop_time);
    printf("Postprocessing Time: %.3f msec\n", time_post);

    //Save Image
    imge->save(save_filename);

    g_ort->ReleaseValue(input_tensor_image);
    g_ort->ReleaseValue(input_tensor_shape);
    g_ort->ReleaseValue(output_tensor_boxes);
    g_ort->ReleaseValue(output_tensor_scores);
    g_ort->ReleaseValue(output_tensor_classes);
    
    
    printf("\x1b[36;1m");
    printf("Prediction Time: %.3f msec\n\n", diff);
    printf("\x1b[0m");

    delete imge;
    g_ort->ReleaseSession(session);
    g_ort->ReleaseSessionOptions(session_options);
    g_ort->ReleaseEnv(env);


    printf("Done!\n");

    return 0;
}
