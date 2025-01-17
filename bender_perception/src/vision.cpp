#include <bender_perception/vision.h>


LaneDetection::LaneDetection(ros::NodeHandle *nh, int device_id) :
    device_id_(device_id),
    input_topic_(""),
    it_(*nh),
    tf_listener_(tf_buffer_)
{
    // Starts capture device
    cam_capture_.open(device_id_);
    if (!cam_capture_.isOpened()) {
        ROS_ERROR("Cannot open camera ID %d", device_id_);
        nh->shutdown();
    }

    // Read image and store it to img_src_
    cam_capture_.read(img_src_);
    if (img_src_.empty()) {
        ROS_ERROR("Cannot read from camera ID %d", device_id_);
        nh->shutdown();
    } 
    init(nh);

}


LaneDetection::LaneDetection(ros::NodeHandle *nh, string input_topic, string output_topic) :
    device_id_(UINT8_MAX),
    input_topic_(input_topic),
    output_topic_(output_topic),
    it_(*nh),
    tf_listener_(tf_buffer_)
{
    // Subscribe to input image
    std::string topic = nh->resolveName(input_topic_);
    input_sub_ = it_.subscribeCamera(topic, 1, &LaneDetection::readImage, this);
    init(nh);
}


void LaneDetection::init(ros::NodeHandle *nh)
{
    output_pub_ = it_.advertise(output_topic_, 1);
    scan_pub_ = nh->advertise<sensor_msgs::LaserScan>("scan_from_image", 1);
    btl_.set_output_frame("logitech_cam_sensor");
}


LaneDetection::~LaneDetection()
{
}


void LaneDetection::readImage()
{
    cam_capture_.read(img_src_);
}


void LaneDetection::readImage(const sensor_msgs::ImageConstPtr &img_msg, 
                              const sensor_msgs::CameraInfoConstPtr& info_msg)
{
    // Get camera model that produced the ROS image msg
    cam_model_.fromCameraInfo(info_msg);
    
    // Convert incoming ROS image msg to OpenCV msg
    cv_bridge::CvImagePtr cv_ptr;
    try
    {
        cv_ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::BGR8);
        img_src_ = cv_ptr->image;
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("Failed to convert image. cv_bridge exception: %s", e.what());
        return;
    }
}

void LaneDetection::gammaCorrection()
{
    const double gamma = 3.0;
    Mat lookUpTable(1, 256, CV_8U);
    uchar* p = lookUpTable.ptr();
    for (int i = 0; i < 256; ++i)
    {
        p[i] = saturate_cast<uchar>(pow(i / 255.0, gamma) * 255.0);
    }
    LUT(img_out_, lookUpTable, img_out_);
}


void LaneDetection::smooth()
{
    /* 
    stackoverflow.com/questions/42065405
    smooth the image with alternative closing and opening
    with an enlarging kernel
    */
    Mat morph = img_out_.clone();
    for (int r = 1; r < 3; r++)
    {
        Mat kernel = getStructuringElement(MORPH_ELLIPSE, Size(2*r+1, 2*r+1));
        morphologyEx(morph, morph, CV_MOP_CLOSE, kernel);
        morphologyEx(morph, morph, CV_MOP_OPEN, kernel);
    }
    img_out_ = morph;
    /* take morphological gradient */
    // Mat mgrad;
    // Mat kernel = getStructuringElement(MORPH_ELLIPSE, Size(3, 3));
    // morphologyEx(morph, mgrad, CV_MOP_GRADIENT, kernel);

    // Mat ch[3];
    // /* split the gradient image into channels */
    // split(mgrad, ch);
    // /* apply Otsu threshold to each channel */
    // threshold(ch[0], ch[0], 0, 255, CV_THRESH_BINARY | CV_THRESH_OTSU);
    // threshold(ch[1], ch[1], 0, 255, CV_THRESH_BINARY | CV_THRESH_OTSU);
    // threshold(ch[2], ch[2], 0, 255, CV_THRESH_BINARY | CV_THRESH_OTSU);
    // /* merge the channels */
    // merge(ch, 3, img_out_);

    // threshold(mgrad, mgrad, 0, 255, THRESH_BINARY | THRESH_OTSU);
    // img_out_ = mgrad;
}


void LaneDetection::quantize()
{
    // Convert to float & reshape to a [3 x W*H] Mat 
    // (so every pixel is on a row of it's own)
    Mat data;
    img_out_.convertTo(data, CV_32F);
    data = data.reshape(1, data.total());
    
    // Mat labels, centers;
    const int init_method = has_centers_ ? KMEANS_USE_INITIAL_LABELS | KMEANS_PP_CENTERS : KMEANS_PP_CENTERS;
    double compactness = kmeans(
        data,
        this->num_colors, 
        labels_,
        TermCriteria( TermCriteria::EPS+TermCriteria::COUNT, 10, 0.5 ),
        2, 
        init_method,
        centers_
    );
    has_centers_ = true;

    // reshape both to a single row of Vec3f pixels:
    centers_ = centers_.reshape(3, centers_.rows);
    data = data.reshape(3, data.rows);

    // replace pixel values with their center value:
    Vec3f *p = data.ptr<Vec3f>();
    for (size_t i=0; i<data.rows; i++) {
        int center_id = labels_.at<int>(i);
        p[i] = centers_.at<Vec3f>(center_id);
    }

    // back to 2d, and uchar:
    img_out_ = data.reshape(3, img_out_.rows);
    img_out_.convertTo(img_out_, CV_8U);
}


void LaneDetection::toBinary()
{
    cvtColor(img_out_, img_out_, CV_BGR2GRAY);
    threshold(img_out_, img_out_, 0, 255, THRESH_BINARY | THRESH_OTSU);
    bitwise_not(img_out_, img_out_);
#ifdef HAVE_OPENCV_XIMGPROC
    // ximgproc::thinning(img_out_, img_out_);
#endif
}


void LaneDetection::update()
{
    if (input_topic_.empty()) 
    {
        readImage();
    }
    
    if (!img_src_.empty())
    {
        if (!has_homography_)
        {

	    Size image_size = img_src_.size();
    	    double w = (double)image_size.width;
    	    double h = (double)image_size.height;
    
	    computeHomography();
            has_homography_ = true;
        }
        if (!mask_created) 
        {
            mask = Mat::zeros(h, w, CV_8UC1);
            rectangle(mask, Point(0, 0), Point(640, 350), Scalar(255), FILLED, LINE_8);
            rectangle(mask, Point(0, 350), Point(150, 480), Scalar(255), FILLED, LINE_8);
            rectangle(mask, Point(490, 350), Point(640, 480), Scalar(255), FILLED, LINE_8);

            mask_created = true;
        }
        // copy the origianl source into igm_out_
        img_src_.copyTo(img_out_);
        
        //Apply the mask to take out the robot
        img_out_.copyTo(img_out_, mask);
        
        //Blur the image to help smooth the lines
        GaussianBlur(img_out_, img_out_, Size(5, 5), 0, 0);
        
        // convert the image to HLS since L is most closely associated with white
        cvtColor(img_out_, img_out_, COLOR_BGR2HLS, 0);
       
        // gamma correction will use a mon-linear scale to separate the values of HLS  - higher values less speration.
        gammaCorrection();
        
        // Determine the mean of L in the resulting image, This will tell us how btight the image is and help set the threshold for low_L
        const auto result = mean(img_out_);
        low_L = int(result(1));
        
        //perform an HLS threshold on the image, only keeps values between the ranges. Outputs a binary file
        Mat hls_threshold;
        inRange(img_out_, Scalar(low_H, low_L, low_S), Scalar(high_H, high_L, high_S), hls_threshold); 
        
        // Determine if the threshold was too low due to the image being overexposed. If so increase low_l, until the white is only about 7% of image
        int numberWhite = countNonZero(hls_threshold);
        while (numberWhite > 20000) {
            low_L = low_L + 5;
            inRange(img_out_, Scalar(low_H, low_L, low_S), Scalar(high_H, high_L, high_S), hls_threshold);
            numberWhite = countNonZero(hls_threshold);
        }
        
        // Dialate the image to fill in gaps
        Mat dilation_dst;
        Mat element = getStructuringElement(MORPH_RECT, Size(5, 5), Point(-1, 1));
        dilate(hls_threshold, dilation_dst, element);
        
        
        //find contours in the image
        vector<vector<Point> > contours;
	findContours(dilation_dst, contours, RETR_EXTERNAL, CHAIN_APPROX_NONE);

        
        // filter the contours by area, too small or too big get tossed
	vector<vector<Point> > goodcontours;
	double minArea = 1500;
	double maxArea = 100000;
	for (size_t i = 0; i < contours.size(); i++)
	{
		double area = cv::contourArea(contours[i]);
		if (area >= minArea && area <= maxArea) 
		{
			goodcontours.push_back(contours.at(i));
		}
	}
        
        
        // creat a new image of these contours
        Mat draw_countours = Mat::zeros(hls_threshold.size(), CV_8UC1);
        for (size_t i = 0; i < goodcontours.size(); i++)
	{

		drawContours(draw_countours, goodcontours, (int)i, 255, 3, LINE_8);
	}
        draw_countours.copyTo(img_out_);
        
        // toBinary();
        // copyMakeBorder(img_out_, img_out_, roi_from_top, roi_from_bot, 0, 0, BORDER_CONSTANT, 0);
        projectToGrid();
	displayOutput();    
    } 
    else
    {
        ROS_INFO_THROTTLE(2.0, "Waiting for input topic %s", input_topic_.c_str());
    }
}


void LaneDetection::computeHomography()
{

    // TODO: Load these from a yaml file
    double alpha = (15-90) * M_PI / 180.0;
    double beta = 0;
    double gamma = 0; 
    double dist = 0.6;

    // Projecion matrix 2D -> 3D
    Matx43d A1(
        1, 0, -w/2,
        0, 1, -h/2,
        0, 0, 0,
        0, 0, 1
    );

    // Rotation matrices Rx, Ry, Rz
    Matx44d RX(
        1, 0, 0, 0,
        0, cos(alpha), -sin(alpha), 0,
        0, sin(alpha), cos(alpha), 0,
        0, 0, 0, 1
    );
    Matx44d RY(
        cos(beta), 0, sin(beta), 0,
        0, 1, 0, 0,
        -sin(beta), 0, cos(beta), 0,
        0, 0, 0, 1
    );
    Matx44d RZ(
        cos(gamma), -sin(gamma), 0, 0,
        sin(gamma), cos(gamma), 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    );

    // Compose the rotations
    Matx44d R = RX * RY * RZ;

    // T - translation matrix
    Matx44d T(
        1, 0, 0, 0,  
        0, 1, 0, 0,  
        0, 0, 1, dist*cam_model_.fx(),  
        0, 0, 0, 1
    ); 
    
    // K - intrinsic matrix 
    Matx34d K(
        cam_model_.fx(), 0, w/2, 0,
        0, cam_model_.fy(), h/2, 0,
        0, 0, 1, 0
    ); 

    // H - Homography
    H_ = K * (T * (R * A1));
}


void LaneDetection::projectToGrid()
{
    warpPerspective(
        img_out_, 
        img_out_, 
        H_, 
        img_out_.size(), 
        INTER_CUBIC | WARP_INVERSE_MAP
    );
}


void LaneDetection::displayOutput()
{
    if (!img_src_.empty() && !img_out_.empty())
    {
        Mat out, display;
        resize(img_out_, out, img_src_.size());
        hconcat(img_src_, out, display);
        namedWindow(wname_, WINDOW_AUTOSIZE);
        imshow(wname_, display);
        waitKey(1);
    }
}


void LaneDetection::publishQuantized()
{
    if (!img_out_.empty()) 
    {
        std::string encoding = typeToString(img_out_.type()).substr(3);
        output_msg_ = cv_bridge::CvImage(std_msgs::Header(), encoding.c_str(), img_out_).toImageMsg();
        output_pub_.publish(output_msg_);
        sensor_msgs::CameraInfoConstPtr info_msg( new sensor_msgs::CameraInfo(cam_model_.cameraInfo()) );
        sensor_msgs::LaserScanPtr scan_msg = btl_.convert_msg(output_msg_, info_msg);
        scan_pub_.publish(scan_msg);
    }
}
