// Remote Gaming App using C++
#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <opencv2/opencv.hpp>
#include <aom/aom_codec.h>
#include <aom/aom_encoder.h>
#include <aom/aom_decoder.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <linux/input.h>
#include <fcntl.h>
#include <json/json.h>
#include <chrono>

#define BUFFER_SIZE 4096

using namespace cv;
using namespace std;
using namespace chrono;

void handle_input(const string& input_data) {
    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(input_data, root)) {
        cerr << "Failed to parse input data" << endl;
        return;
    }

    string type = root["type"].asString();
    if (type == "mouse") {
        int x = root["x"].asInt();
        int y = root["y"].asInt();
        bool left_click = root["left_click"].asBool();
        bool right_click = root["right_click"].asBool();

        Display* display = XOpenDisplay(NULL);
        if (display) {
            XTestFakeMotionEvent(display, -1, x, y, CurrentTime);
            if (left_click) {
                XTestFakeButtonEvent(display, 1, True, CurrentTime);
                XTestFakeButtonEvent(display, 1, False, CurrentTime);
            }
            if (right_click) {
                XTestFakeButtonEvent(display, 3, True, CurrentTime);
                XTestFakeButtonEvent(display, 3, False, CurrentTime);
            }
            XCloseDisplay(display);
        }
    } else if (type == "keyboard") {
        string key = root["key"].asString();
        bool press = root["press"].asBool();

        int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            struct input_event event = {};
            event.type = EV_KEY;
            event.code = key[0]; // Simplified mapping
            event.value = press ? 1 : 0;
            write(fd, &event, sizeof(event));

            event.type = EV_SYN;
            event.code = SYN_REPORT;
            event.value = 0;
            write(fd, &event, sizeof(event));

            close(fd);
        }
    }
}

int detect_client_refresh_rate() {
    // Placeholder: assume 60Hz if detection is not implemented
    // You can implement detection by querying the client's display hardware
    return 60;
}

void server_stream(const char* host, int port) {
    // Server socket setup
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        cerr << "Socket creation failed" << endl;
        exit(EXIT_FAILURE);
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(host);
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "Socket bind failed" << endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Screen capture setup (dynamic resolution detection)
    Size screen_size = Size(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    Mat frame;

    // Detect refresh rate
    int refresh_rate = detect_client_refresh_rate();

    // AV1 encoder setup
    aom_codec_ctx_t codec;
    aom_codec_enc_cfg_t cfg;

    aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg, 0);
    cfg.g_w = screen_size.width;
    cfg.g_h = screen_size.height;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = refresh_rate;
    cfg.rc_target_bitrate = 5000;

    if (aom_codec_enc_init(&codec, aom_codec_av1_cx(), &cfg, 0) != AOM_CODEC_OK) {
        cerr << "Failed to initialize encoder" << endl;
        exit(EXIT_FAILURE);
    }

    auto frame_interval = duration<double>(1.0 / refresh_rate);
    auto next_frame_time = steady_clock::now();

    while (true) {
        next_frame_time += frame_interval;

        // Capture screen (mockup using a blank image)
        frame = Mat::zeros(screen_size, CV_8UC3);
        putText(frame, "Remote Gaming", Point(50, 50), FONT_HERSHEY_SIMPLEX, 1, Scalar(255, 255, 255), 2);

        // Encode frame
        vector<uint8_t> encoded_frame;
        uint8_t* img_data = frame.data;
        aom_image_t img;
        aom_img_wrap(&img, AOM_IMG_FMT_I420, frame.cols, frame.rows, 1, img_data);

        if (aom_codec_encode(&codec, &img, 0, 1, 0) != AOM_CODEC_OK) {
            cerr << "Encoding failed" << endl;
            continue;
        }

        const aom_codec_cx_pkt_t* pkt;
        while ((pkt = aom_codec_get_cx_data(&codec, NULL))) {
            if (pkt->kind == AOM_CODEC_CX_FRAME_PKT) {
                sendto(server_fd, pkt->data.frame.buf, pkt->data.frame.sz, 0, (sockaddr*)&server_addr, sizeof(server_addr));
            }
        }

        this_thread::sleep_until(next_frame_time);
    }

    aom_codec_destroy(&codec);
    close(server_fd);
}

void client_stream(const char* host, int port) {
    // Client socket setup
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_fd < 0) {
        cerr << "Socket creation failed" << endl;
        exit(EXIT_FAILURE);
    }

    sockaddr_in client_addr{};
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(host);
    client_addr.sin_port = htons(port);

    if (bind(client_fd, (sockaddr*)&client_addr, sizeof(client_addr)) < 0) {
        cerr << "Socket bind failed" << endl;
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    // AV1 decoder setup
    aom_codec_ctx_t codec;
    if (aom_codec_dec_init(&codec, aom_codec_av1_dx(), NULL, 0) != AOM_CODEC_OK) {
        cerr << "Failed to initialize decoder" << endl;
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    while (true) {
        ssize_t len = recvfrom(client_fd, buffer, BUFFER_SIZE, 0, NULL, NULL);
        if (len <= 0) continue;

        if (len < BUFFER_SIZE - 1) buffer[len] = '\0';
        string input_data(buffer);

        if (input_data.find("type") != string::npos) {
            handle_input(input_data);
            continue;
        }

        aom_codec_err_t res = aom_codec_decode(&codec, (uint8_t*)buffer, len, NULL);
        if (res != AOM_CODEC_OK) {
            cerr << "Decoding failed" << endl;
            continue;
        }

        aom_image_t* img = aom_codec_get_frame(&codec, NULL);
        if (img) {
            Mat frame(Size(img->d_w, img->d_h), CV_8UC3, img->planes[0]);
            imshow("Remote Gaming", frame);
            if (waitKey(1) == 'q') break;
        }
    }

    aom_codec_destroy(&codec);
    close(client_fd);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        cerr << "Usage: ./remote_gaming server|client <IP> <PORT>" << endl;
        return -1;
    }

    string mode = argv[1];
    const char* host = argv[2];
    int port = stoi(argv[3]);

    if (mode == "server") {
        server_stream(host, port);
    } else if (mode == "client") {
        client_stream(host, port);
    } else {
        cerr << "Invalid mode. Use 'server' or 'client'." << endl;
        return -1;
    }

    return 0;
}
