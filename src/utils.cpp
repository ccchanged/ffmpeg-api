#include "utils.hpp"

extern "C" {
    #include <libavutil/error.h>
}

namespace ffmpeg::utils {

std::string getErrorString(int errorCode) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errorCode, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return std::string(errbuf);
}

}
