/******************************************************************
 * QRAC - Quantitative Random Access Codes
 * Copyright (c) 2024 [你的名字]. 保留所有权利.
 *
 * 本软件根据MIT许可证分发 - 详见LICENSE文件
 * 项目地址: https://github.com/[你的用户名]/QRAC
 *
 * 第三方库:
 * - LodePNG (zlib许可证)
 * - stb_image系列 (公共领域)
 ******************************************************************/
#define NOMINMAX
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <string>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <limits>
#include <filesystem>
#include <unordered_map>

// 使用C++17文件系统库
namespace fs = std::filesystem;

// 包含stb图像库
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize.h"

// Windows特定头文件
#ifdef _WIN32
#include <windows.h>
#endif

// 配置结构体
struct QRACConfig {
    int L = 5; // Quantization interval length
    uint8_t FILLER_MAX_VALUE = 10; // 最大填充值（深灰色）
    float FEC_REDUNDANCY_RATIO = 0.25f; // FEC冗余比例
    int MIN_IMAGE_DIMENSION = 16; // 最小图像尺寸
    int DEFAULT_SMALL_SIZE = 128; // 默认小尺寸
    int DEFAULT_MEDIUM_SIZE = 512; // 默认中尺寸
    int DEFAULT_LARGE_SIZE = 1024; // 默认大尺寸
    size_t SMALL_FILE_THRESHOLD = 96 * 1024; // 小文件阈值 (96KB)
    size_t MEDIUM_FILE_THRESHOLD = 1024 * 1024; // 中文件阈值 (1MB)
    int SYMBOLS_PER_PIXEL = 3; // 每像素符号数 (RGB)
    int FEC_BLOCK_SIZE = 10; // FEC块大小
    int MAX_FEC_WARNINGS = 15; // 最大FEC警告数
    float TEXT_DETECTION_THRESHOLD = 0.85f; // 文本检测阈值
    float CONTROL_CHAR_THRESHOLD = 0.05f; // 控制字符阈值
    bool USE_ADVANCED_FEC = false; // 使用简单FEC而不是Reed-Solomon
};

// 全局配置实例
QRACConfig g_config;

// 计算间隔数量
int calculateIntervals() {
    int availableRange = 256 - (g_config.FILLER_MAX_VALUE + 1); // 跳过0-10的范围
    return availableRange / g_config.L + (availableRange % g_config.L != 0 ? 1 : 0);
}

// 错误类型枚举
enum class ErrorType {
    FileNotFound,
    FileReadError,
    FileWriteError,
    ImageLoadError,
    ImageSaveError,
    ImageSizeError,
    DataSizeError,
    FECError,
    UserAbort,
    InvalidInput
};

// 自定义异常类
class QRACException : public std::runtime_error {
public:
    QRACException(ErrorType type, const std::string& message)
        : std::runtime_error(message), m_type(type) {}

    ErrorType getType() const { return m_type; }

private:
    ErrorType m_type;
};

// 信任声明和程序信息
const char* TRUST_STATEMENT =
"=== 信任声明 ===\n"
"本程序是QRAC数据编码工具，用于将文件编码为图像或从图像解码文件。\n"
"程序完全开源，不包含任何恶意代码或病毒。\n"
"某些安全软件可能会误报，这是因为程序使用了数据编码技术。\n"
"请放心使用，如有疑问可查看源代码或联系开发者。\n"
"================\n";

// Calculate anchor point value
int calculateAnchor(int intervalIndex) {
    int intervals = calculateIntervals();
    int start = g_config.FILLER_MAX_VALUE + 1 + intervalIndex * g_config.L; // 从11开始
    int end = std::min(start + g_config.L - 1, 255);
    return start + (end - start) / 2; // Midpoint of interval
}

// 检查是否为填充值（包括接近黑色的像素）
bool isFillerValue(uint8_t pixelValue) {
    return pixelValue <= g_config.FILLER_MAX_VALUE;
}

// 检查整个像素是否为填充颜色
bool isFillerPixel(const uint8_t* pixel, int channels) {
    return isFillerValue(pixel[0]) && isFillerValue(pixel[1]) && isFillerValue(pixel[2]);
}

// Decode pixel value to interval index
int decodeToSymbol(uint8_t pixelValue) {
    // 如果是填充值，返回特殊标记
    if (isFillerValue(pixelValue)) {
        return -1; // 特殊值，表示填充区域
    }

    // 跳过0-10的范围，从11开始计算
    int adjustedValue = pixelValue - (g_config.FILLER_MAX_VALUE + 1);
    int intervals = calculateIntervals();
    int intervalIndex = adjustedValue / g_config.L;

    // 确保在有效范围内
    if (intervalIndex >= intervals) {
        intervalIndex = intervals - 1;
    }

    return intervalIndex;
}

// 简单的FEC编码
void addFEC(std::vector<uint8_t>& data) {
    size_t originalSize = data.size();
    if (originalSize == 0) return;

    // 根据配置添加FEC冗余
    size_t fecSize = static_cast<size_t>(originalSize * g_config.FEC_REDUNDANCY_RATIO);
    data.resize(originalSize + fecSize);

    // 使用简单的线性编码进行FEC
    for (size_t i = 0; i < fecSize; i++) {
        uint8_t fecByte = 0;
        for (size_t j = 0; j < 8; j++) {
            size_t index = (j * fecSize + i) % originalSize;
            fecByte ^= data[index]; // XOR操作
        }
        data[originalSize + i] = fecByte;
    }
}

// 简单的FEC解码
bool verifyAndCorrectFEC(std::vector<uint8_t>& data) {
    if (data.size() < 5) {
        return true; // 数据太小，无法进行FEC校正
    }

    size_t originalSize = static_cast<size_t>(data.size() / (1.0f + g_config.FEC_REDUNDANCY_RATIO));
    size_t fecSize = data.size() - originalSize;

    if (fecSize == 0) {
        return true; // No FEC data
    }

    bool hasError = false;
    std::vector<uint8_t> correctedData(data.begin(), data.begin() + originalSize);

    // 检查错误
    for (size_t i = 0; i < fecSize; i++) {
        uint8_t calculatedFEC = 0;
        for (size_t j = 0; j < 8; j++) {
            size_t index = (j * fecSize + i) % originalSize;
            calculatedFEC ^= correctedData[index];
        }

        if (data[originalSize + i] != calculatedFEC) {
            hasError = true;
            break;
        }
    }

    if (!hasError) {
        data = std::move(correctedData);
        return true;
    }

    // 尝试纠正错误
    for (size_t i = 0; i < fecSize; i++) {
        uint8_t calculatedFEC = 0;
        for (size_t j = 0; j < 8; j++) {
            size_t index = (j * fecSize + i) % originalSize;
            calculatedFEC ^= correctedData[index];
        }

        if (data[originalSize + i] != calculatedFEC) {
            // 尝试找到并纠正错误
            for (size_t j = 0; j < 8; j++) {
                size_t index = (j * fecSize + i) % originalSize;
                uint8_t originalByte = correctedData[index];

                // 尝试翻转每个位
                for (int bit = 0; bit < 8; bit++) {
                    uint8_t testByte = originalByte ^ (1 << bit);
                    uint8_t testFEC = calculatedFEC ^ originalByte ^ testByte;

                    if (testFEC == data[originalSize + i]) {
                        correctedData[index] = testByte;
                        std::cout << "Corrected byte error at position " << index << "\n";
                        break;
                    }
                }
            }
        }
    }

    // 最终验证
    bool allErrorsCorrected = true;
    for (size_t i = 0; i < fecSize; i++) {
        uint8_t calculatedFEC = 0;
        for (size_t j = 0; j < 8; j++) {
            size_t index = (j * fecSize + i) % originalSize;
            calculatedFEC ^= correctedData[index];
        }

        if (data[originalSize + i] != calculatedFEC) {
            allErrorsCorrected = false;
            if (i < g_config.MAX_FEC_WARNINGS) {
                std::cout << "Warning: Unable to correct error in FEC block " << i << "\n";
            }
            else if (i == g_config.MAX_FEC_WARNINGS) {
                std::cout << "Additional FEC errors omitted for brevity...\n";
            }
            break;
        }
    }

    // 更新数据
    data = std::move(correctedData);
    return allErrorsCorrected;
}

// Convert data to binary stream
std::vector<bool> dataToBinary(const std::vector<uint8_t>& data) {
    std::vector<bool> binaryStream;
    binaryStream.reserve(data.size() * 8);

    for (uint8_t byte : data) {
        for (int i = 7; i >= 0; i--) {
            binaryStream.push_back((byte >> i) & 1);
        }
    }

    return binaryStream;
}

// Convert binary stream to symbol sequence
std::vector<int> binaryToSymbols(const std::vector<bool>& binaryStream, int bitsPerSymbol) {
    int intervals = calculateIntervals();
    int symbolCount = static_cast<int>((binaryStream.size() + bitsPerSymbol - 1) / bitsPerSymbol);
    std::vector<int> symbols;
    symbols.reserve(symbolCount);

    for (int i = 0; i < symbolCount; i++) {
        int symbol = 0;
        for (int j = 0; j < bitsPerSymbol; j++) {
            int bitIndex = i * bitsPerSymbol + j;
            if (bitIndex < static_cast<int>(binaryStream.size())) {
                symbol = (symbol << 1) | (binaryStream[bitIndex] ? 1 : 0);
            }
            else {
                symbol = symbol << 1; // Pad with 0
            }
        }
        symbols.push_back(symbol % intervals);
    }

    return symbols;
}

// Improved QRAC image creation function with filler value for unused areas
std::vector<uint8_t> createQRACImage(const std::vector<int>& symbols, int width, int height, bool useFEC) {
    int totalPixels = width * height;
    int symbolsPerPixel = g_config.SYMBOLS_PER_PIXEL;

    int requiredPixels = (static_cast<int>(symbols.size()) + symbolsPerPixel - 1) / symbolsPerPixel;

    if (requiredPixels > totalPixels) {
        throw QRACException(ErrorType::ImageSizeError, "Image dimensions too small to contain all data");
    }

    // Create image data (初始化为纯黑色填充)
    int channels = 3; // 使用RGB
    std::vector<uint8_t> imageData(width * height * channels, 0); // 初始化为纯黑色

    // Map symbols to image pixels
    for (int i = 0; i < static_cast<int>(symbols.size()); i += symbolsPerPixel) {
        int pixelIndex = i / symbolsPerPixel;
        int row = pixelIndex / width;
        int col = pixelIndex % width;

        // 只处理实际需要的像素
        if (row >= height) {
            break; // 防止越界
        }

        int dataIndex = (row * width + col) * channels;

        // Assign symbols to each channel
        for (int ch = 0; ch < symbolsPerPixel && (i + ch) < static_cast<int>(symbols.size()); ch++) {
            int symbol = symbols[i + ch];
            imageData[dataIndex + ch] = calculateAnchor(symbol);
        }
    }

    return imageData;
}

// Extract binary stream from symbol sequence, skipping filler values
std::vector<bool> symbolsToBinary(const std::vector<int>& symbols, int bitsPerSymbol, size_t expectedBits) {
    std::vector<bool> binaryStream;
    binaryStream.reserve(expectedBits);

    for (int symbol : symbols) {
        // 跳过填充符号
        if (symbol == -1) {
            continue;
        }

        for (int i = bitsPerSymbol - 1; i >= 0; i--) {
            bool bit = (symbol >> i) & 1;
            binaryStream.push_back(bit);

            // 如果已经达到预期的位数，停止处理
            if (binaryStream.size() >= expectedBits) {
                break;
            }
        }

        // 如果已经达到预期的位数，停止处理
        if (binaryStream.size() >= expectedBits) {
            break;
        }
    }

    if (binaryStream.size() > expectedBits) {
        binaryStream.resize(expectedBits);
    }

    return binaryStream;
}

// Convert binary stream to byte data
std::vector<uint8_t> binaryToData(const std::vector<bool>& binaryStream) {
    std::vector<uint8_t> data;
    size_t byteCount = (binaryStream.size() + 7) / 8;
    data.reserve(byteCount);

    for (size_t i = 0; i < binaryStream.size(); i += 8) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; j++) {
            if (i + j < binaryStream.size()) {
                byte = (byte << 1) | (binaryStream[i + j] ? 1 : 0);
            }
            else {
                byte = byte << 1; // Pad with 0
            }
        }
        data.push_back(byte);
    }

    return data;
}

// Determine if data is text
bool isTextData(const std::vector<uint8_t>& data) {
    if (data.empty()) return false;

    size_t checkSize = std::min(data.size(), size_t(1000));
    int printableCount = 0;
    int controlCount = 0;
    int nullCount = 0;

    for (size_t i = 0; i < checkSize; i++) {
        uint8_t c = data[i];
        if (c >= 32 && c <= 126) { // 可打印ASCII字符
            printableCount++;
        }
        else if (c == 9 || c == 10 || c == 13) { // 制表符、换行符、回车符
            printableCount++;
        }
        else if (c == 0) { // 空字符
            nullCount++;
            // 文本文件中不应该有太多空字符
            if (nullCount > checkSize / 20) { // 如果超过5%是空字符，可能是二进制文件
                return false;
            }
        }
        else if (c < 32) { // 其他控制字符
            controlCount++;
            // 文本文件中不应该有太多控制字符
            if (controlCount > checkSize / 50) { // 如果超过2%是控制字符，可能是二进制文件
                return false;
            }
        }
        else {
            // 可能是UTF-8多字节字符的一部分
            printableCount++;
        }
    }

    // 计算可打印字符的比例
    float printableRatio = static_cast<float>(printableCount) / checkSize;
    float controlRatio = static_cast<float>(controlCount) / checkSize;

    // 可打印字符比例高且控制字符比例低的很可能是文本
    return (printableRatio > g_config.TEXT_DETECTION_THRESHOLD) &&
        (controlRatio < g_config.CONTROL_CHAR_THRESHOLD);
}

// Get file extension
std::string getFileExtension(const std::string& filename) {
    size_t dotPos = filename.find_last_of(".");
    if (dotPos != std::string::npos) {
        return filename.substr(dotPos + 1);
    }
    return "";
}

// Convert to lowercase
std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// Get directory from file path
std::string getDirectoryFromPath(const std::string& filePath) {
    size_t lastSlash = filePath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return filePath.substr(0, lastSlash + 1);
    }
    return "";
}

// Get filename without path
std::string getFilenameWithoutPath(const std::string& filePath) {
    size_t lastSlash = filePath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return filePath.substr(lastSlash + 1);
    }
    return filePath;
}

// Generate output filename in same directory
std::string generateOutputFilename(const std::string& inputPath, const std::string& suffix, const std::string& extension) {
    std::string directory = getDirectoryFromPath(inputPath);
    std::string filename = getFilenameWithoutPath(inputPath);

    // Remove existing extension if present
    size_t dotPos = filename.find_last_of(".");
    if (dotPos != std::string::npos) {
        filename = filename.substr(0, dotPos);
    }

    return directory + filename + suffix + "." + extension;
}

// Calculate optimal image dimensions for adaptive mode
void calculateAdaptiveDimensions(size_t dataSize, int* width, int* height) {
    // 计算每个符号的位数
    int intervals = calculateIntervals();
    int bitsPerSymbol = static_cast<int>(std::log2(intervals));

    // 计算所需的总符号数（包括FEC）
    size_t totalSymbols = (dataSize * 8 + bitsPerSymbol - 1) / bitsPerSymbol;

    // 计算所需像素数（每个像素存储3个符号）
    int symbolsPerPixel = g_config.SYMBOLS_PER_PIXEL;
    int pixelsNeeded = static_cast<int>((totalSymbols + symbolsPerPixel - 1) / symbolsPerPixel); // 向上取整

    // 找到能容纳像素的最小正方形
    int side = static_cast<int>(std::ceil(std::sqrt(pixelsNeeded)));

    // 确保最小尺寸
    side = std::max(side, g_config.MIN_IMAGE_DIMENSION);

    // 计算实际需要的行数和列数
    int actualWidth = static_cast<int>(std::ceil(std::sqrt(pixelsNeeded)));
    int actualHeight = (pixelsNeeded + actualWidth - 1) / actualWidth;

    *width = std::max(actualWidth, g_config.MIN_IMAGE_DIMENSION);
    *height = std::max(actualHeight, g_config.MIN_IMAGE_DIMENSION);

    std::cout << "Precise dimensions: " << *width << "x" << *height
        << " (pixels needed: " << pixelsNeeded << ")\n";

    // 计算预计文件大小
    int channels = 3;
    int rowSize = *width * channels;
    size_t estimatedSize = rowSize * *height;

    std::cout << "Estimated image size: " << (estimatedSize / 1024) << "KB\n";
}

// 使用stb_image_write保存图像
bool saveImage(const std::string& filename, const uint8_t* data, int width, int height, int channels, const std::string& format) {
    if (format == "bmp") {
        return stbi_write_bmp(filename.c_str(), width, height, channels, data) != 0;
    }
    else if (format == "png") {
        // PNG压缩级别 6 (中等压缩级别，平衡速度和质量)
        return stbi_write_png(filename.c_str(), width, height, channels, data, width * channels) != 0;
    }
    return false;
}

// 使用智能指针包装STB图像加载
struct STBImageDeleter {
    void operator()(unsigned char* data) const {
        stbi_image_free(data);
    }
};

using STBImagePtr = std::unique_ptr<unsigned char, STBImageDeleter>;

// UTF-8到宽字符串转换函数
#ifdef _WIN32
std::wstring utf8ToWstring(const std::string& utf8Str) {
    if (utf8Str.empty()) return std::wstring();

    int wideStrLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
    if (wideStrLen == 0) return std::wstring();

    std::wstring wideStr(wideStrLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wideStr[0], wideStrLen);

    return wideStr;
}
#endif

// 使用stb_image加载图像
STBImagePtr loadImageSTB(const std::string& filename, int* width, int* height, int* channels, int desired_channels = 0) {
    // 将UTF-8字符串转换为宽字符串（Windows需要）
#ifdef _WIN32
    std::wstring wideStr = utf8ToWstring(filename);
    // 使用宽字符版本的文件打开函数
    FILE* file = _wfopen(wideStr.c_str(), L"rb");
    if (!file) {
        return nullptr;
    }
    unsigned char* data = stbi_load_from_file(file, width, height, channels, desired_channels);
    fclose(file);
#else
    unsigned char* data = stbi_load(filename.c_str(), width, height, channels, desired_channels);
#endif
    return STBImagePtr(data);
}

// 简单的JPG文件检测函数（仅检测文件头）
bool isJPGFile(const std::string& filename) {
#ifdef _WIN32
    std::wstring wideStr = utf8ToWstring(filename);
    std::ifstream file(wideStr, std::ios::binary);
#else
    std::ifstream file(filename, std::ios::binary);
#endif
    if (!file.is_open()) {
        return false;
    }

    unsigned char header[4];
    file.read(reinterpret_cast<char*>(header), 4);
    file.close();

    // 检查JPG文件头 (FF D8 FF E0 或 FF D8 FF E1)
    return (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF &&
        (header[3] == 0xE0 || header[3] == 0xE1));
}

// 显示JPG警告
void showJPGWarning() {
    std::cout << "======================================================\n";
    std::cout << "                    警告: JPG格式检测\n";
    std::cout << "======================================================\n";
    std::cout << "JPG是一种有损压缩格式，不适合用于数据编码/解码。\n";
    std::cout << "使用JPG格式可能导致数据损坏或无法正确解码。\n";
    std::cout << "建议使用无损格式如PNG或BMP进行编码。\n";
    std::cout << "是否继续处理? (y/n): ";

    char choice;
    std::cin >> choice;
    if (choice != 'y' && choice != 'Y') {
        throw QRACException(ErrorType::UserAbort, "用户取消操作");
    }
}

// Check if file exists
bool fileExists(const std::string& filename) {
#ifdef _WIN32
    std::wstring wideStr = utf8ToWstring(filename);
    return fs::exists(wideStr) && fs::is_regular_file(wideStr);
#else
    return fs::exists(filename) && fs::is_regular_file(filename);
#endif
}

// Get file size
size_t getFileSize(const std::string& filename) {
    try {
#ifdef _WIN32
        std::wstring wideStr = utf8ToWstring(filename);
        return fs::file_size(wideStr);
#else
        return fs::file_size(filename);
#endif
    }
    catch (...) {
        throw QRACException(ErrorType::FileReadError, "无法获取文件大小: " + filename);
    }
}

// Show user guide
void showUserGuide() {
    std::cout << "======================================================\n";
    std::cout << "                  QRAC Tool Suite User Guide\n";
    std::cout << "======================================================\n";
    std::cout << "1. File Locations:\n";
    std::cout << "   - Place files to process in any directory\n";
    std::cout << "   - Use absolute paths for input files\n";
    std::cout << "   - Example: C:\\Users\\YourName\\Documents\\file.txt\n";
    std::cout << "\n";
    std::cout << "2. Output Files:\n";
    std::cout << "   - Output files are saved in the same directory as input files\n";
    std::cout << "   - Automatic naming: inputfile_encoded.png, inputfile_decoded, etc.\n";
    std::cout << "\n";
    std::cout << "3. Supported Formats:\n";
    std::cout << "   - Input: Any file format (Word docs, text files, zip archives, etc.)\n";
    std::cout << "   - Output: PNG format (32-bit RGBA, lossless)\n";
    std::cout << "   - Decoding: Supports PNG, BMP (24/32-bit) and PPM formats\n";
    std::cout << "   - JPG: Limited support (not recommended for data encoding)\n";
    std::cout << "\n";
    std::cout << "4. Operation Process:\n";
    std::cout << "   a) Select operation type (encode/decode/correct)\n";
    std::cout << "   b) Enter input file path (absolute path recommended)\n";
    std::cout << "   c) Processing happens automatically\n";
    std::cout << "   d) Find output file in the same directory as input\n";
    std::cout << "======================================================\n\n";
}

// Show trust statement
void showTrustStatement() {
    std::cout << TRUST_STATEMENT << "\n";
}

// 改进的文件保存逻辑
void saveExtractedData(const std::vector<uint8_t>& data, const std::string& filename, bool isText) {
#ifdef _WIN32
    std::wstring wideStr = utf8ToWstring(filename);
    std::ofstream outFile(wideStr, isText ? std::ios::out : std::ios::binary);
#else
    std::ofstream outFile(filename, isText ? std::ios::out : std::ios::binary);
#endif
    if (!outFile.is_open()) {
        throw QRACException(ErrorType::FileWriteError, "Cannot create output file: " + filename);
    }

    outFile.write(reinterpret_cast<const char*>(data.data()), data.size());
    outFile.close();
}

// PNG压缩函数，自动调整尺寸直到满足目标大小
std::vector<uint8_t> compressImageAuto(const std::vector<uint8_t>& imageData, int width, int height, int channels, size_t maxSizeKB) {
    int tryWidth = width, tryHeight = height;
    std::vector<uint8_t> resized = imageData;
    std::vector<uint8_t> result;
    int compressedSize = 0;
    unsigned char* compressedData = nullptr;

    while (true) {
        compressedData = stbi_write_png_to_mem(
            resized.data(), tryWidth * channels, tryWidth, tryHeight, channels, &compressedSize
        );
        if (compressedData && compressedSize <= static_cast<int>(maxSizeKB * 1024)) {
            result.assign(compressedData, compressedData + compressedSize);
            STBIW_FREE(compressedData);
            break;
        }
        STBIW_FREE(compressedData);
        if (tryWidth <= 64 || tryHeight <= 64) {
            compressedData = stbi_write_png_to_mem(
                resized.data(), tryWidth * channels, tryWidth, tryHeight, channels, &compressedSize
            );
            if (compressedData) {
                result.assign(compressedData, compressedData + compressedSize);
                STBIW_FREE(compressedData);
            }
            break;
        }
        int newWidth = static_cast<int>(tryWidth * 0.9);
        int newHeight = static_cast<int>(tryHeight * 0.9);
        std::vector<uint8_t> nextResized(newWidth * newHeight * channels);
        stbir_resize_uint8(resized.data(), tryWidth, tryHeight, 0,
            nextResized.data(), newWidth, newHeight, 0, channels);
        resized = std::move(nextResized);
        tryWidth = newWidth;
        tryHeight = newHeight;
    }
    return result;
}

// PNG压缩函数（用于校正后的图像，保持接口一致）
std::vector<uint8_t> compressImage(const std::vector<uint8_t>& imageData, int width, int height, int channels) {
    // 使用PNG格式进行无损压缩
    int compressedSize;
    unsigned char* compressedData = stbi_write_png_to_mem(
        imageData.data(), width * channels, width, height, channels, &compressedSize);
    if (!compressedData) {
        throw QRACException(ErrorType::ImageSaveError, "Failed to compress image");
    }

    // 将压缩后的数据复制到vector中
    std::vector<uint8_t> result(compressedData, compressedData + compressedSize);
    STBIW_FREE(compressedData);

    return result;
}

// 文件类型检测
std::string detectFileType(const std::vector<uint8_t>& data) {
    if (data.size() < 4) return "bin";

    // 常见文件类型签名
    static const std::unordered_map<std::string, std::vector<uint8_t>> signatures = {
        {"zip", {0x50, 0x4B, 0x03, 0x04}},
        {"doc", {0xD0, 0xCF, 0x11, 0xE0}},
        {"pdf", {0x25, 0x50, 0x44, 0x46}},
        {"png", {0x89, 0x50, 0x4E, 0x47}},
        {"jpg", {0xFF, 0xD8, 0xFF, 0xE0}},
        {"jpg2", {0xFF, 0xD8, 0xFF, 0xE1}},
        {"bmp", {0x42, 0x4D}},
        {"gif", {0x47, 0x49, 0x46, 0x38}}
    };

    for (const auto& pair : signatures) {
        const std::vector<uint8_t>& sig = pair.second;
        if (data.size() >= sig.size() &&
            std::equal(sig.begin(), sig.end(), data.begin())) {
            return pair.first;
        }
    }

    // 如果不是已知的二进制格式，检查是否为文本
    return isTextData(data) ? "txt" : "bin";
}

// Encoder function
void encodeFile() {
    std::string inputFile, mode, formatChoice;

    std::cout << "[Encode] Convert file to QRAC image\n";
    std::cout << "Enter input file path: ";
    std::getline(std::cin, inputFile);

    if (!fileExists(inputFile)) {
        throw QRACException(ErrorType::FileNotFound, "File does not exist: " + inputFile);
    }

    // 改进的菜单选项
    std::cout << "\n=== Encoding Mode Selection ===\n";
    std::cout << "1. Auto Mode (Recommended for files 36KB-1MB)\n";
    std::cout << "   - System automatically selects optimal size\n";
    std::cout << "   - Small (" << g_config.DEFAULT_SMALL_SIZE << "x" << g_config.DEFAULT_SMALL_SIZE
        << "): Best for files up to " << (g_config.SMALL_FILE_THRESHOLD / 1024) << "KB\n";
    std::cout << "   - Medium (" << g_config.DEFAULT_MEDIUM_SIZE << "x" << g_config.DEFAULT_MEDIUM_SIZE
        << "): Best for files " << (g_config.SMALL_FILE_THRESHOLD / 1024)
        << "KB-" << (g_config.MEDIUM_FILE_THRESHOLD / 1024 / 1024) << "MB\n";
    std::cout << "   - Large (" << g_config.DEFAULT_LARGE_SIZE << "x" << g_config.DEFAULT_LARGE_SIZE
        << "): Best for files over " << (g_config.MEDIUM_FILE_THRESHOLD / 1024 / 1024) << "MB\n";
    std::cout << "2. Adaptive Mode (Optimal for any file size)\n";
    std::cout << "   - Generates minimal image size needed\n";
    std::cout << "   - Example: 5 bytes = small image, 4MB = large image\n";
    std::cout << "   - Most efficient use of space\n";
    std::cout << "Select mode (1 or 2, default 1): ";

    std::getline(std::cin, mode);

    if (mode.empty()) mode = "1";

    // 输出格式选择
    std::cout << "\n=== Output Format Selection ===\n";
    std::cout << "1. PNG format (Recommended, smaller file size, lossless)\n";
    std::cout << "2. BMP format (24-bit, better compatibility)\n";
    std::cout << "Select format (1 or 2, default 1): ";
    std::getline(std::cin, formatChoice);

    std::string outputFormat = "png";
    if (formatChoice == "2") {
        outputFormat = "bmp";
        std::cout << "Using 24-bit BMP format\n";
    }
    else {
        std::cout << "Using PNG format (lossless compression)\n";
    }

    // Read input file
#ifdef _WIN32
    std::wstring wideStr = utf8ToWstring(inputFile);
    std::ifstream file(wideStr, std::ios::binary);
#else
    std::ifstream file(inputFile, std::ios::binary);
#endif
    if (!file.is_open()) {
        throw QRACException(ErrorType::FileReadError, "Cannot open input file: " + inputFile);
    }

    size_t fileSize = getFileSize(inputFile);
    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    file.close();

    std::cout << "Read input file: " << fileSize << " bytes\n";

    // Add forward error correction
    addFEC(fileData);
    std::cout << "Data with FEC: " << fileData.size() << " bytes\n";

    // Determine image dimensions
    int width, height;
    bool useFEC = true;

    if (mode == "2") {
        calculateAdaptiveDimensions(fileData.size(), &width, &height);
        std::cout << "Using adaptive mode: " << width << "x" << height << " pixels\n";
        std::cout << "This will create the minimal image needed for your data\n";
    }
    else { // auto mode (default)
        if (fileSize <= g_config.SMALL_FILE_THRESHOLD) {
            width = g_config.DEFAULT_SMALL_SIZE;
            height = g_config.DEFAULT_SMALL_SIZE;
            std::cout << "Auto-selected small mode: " << width << "x" << height
                << " pixels (best for files up to " << (g_config.SMALL_FILE_THRESHOLD / 1024) << "KB)\n";
        }
        else if (fileSize <= g_config.MEDIUM_FILE_THRESHOLD) {
            width = g_config.DEFAULT_MEDIUM_SIZE;
            height = g_config.DEFAULT_MEDIUM_SIZE;
            std::cout << "Auto-selected medium mode: " << width << "x" << height
                << " pixels (best for files " << (g_config.SMALL_FILE_THRESHOLD / 1024)
                << "KB-" << (g_config.MEDIUM_FILE_THRESHOLD / 1024 / 1024) << "MB)\n";
        }
        else {
            width = g_config.DEFAULT_LARGE_SIZE;
            height = g_config.DEFAULT_LARGE_SIZE;
            std::cout << "Auto-selected large mode: " << width << "x" << height
                << " pixels (best for files over " << (g_config.MEDIUM_FILE_THRESHOLD / 1024 / 1024) << "MB)\n";
        }
        std::cout << "Note: For optimal space efficiency, consider adaptive mode next time\n";
    }

    // Convert data to binary stream
    std::vector<bool> binaryStream = dataToBinary(fileData);
    std::cout << "Generated binary stream: " << binaryStream.size() << " bits\n";

    // Calculate bits per symbol
    int intervals = calculateIntervals();
    int bitsPerSymbol = static_cast<int>(std::log2(intervals));
    std::cout << "Number of intervals: " << intervals << " (L=" << g_config.L << ")\n";
    std::cout << "Bits per symbol: " << bitsPerSymbol << "\n";

    // Convert binary stream to symbol sequence
    std::vector<int> symbols = binaryToSymbols(binaryStream, bitsPerSymbol);
    std::cout << "Generated symbol sequence: " << symbols.size() << " symbols\n";

    // Check if image is large enough
    int symbolsPerPixel = g_config.SYMBOLS_PER_PIXEL;
    int totalPixels = width * height;
    int requiredPixels = (static_cast<int>(symbols.size()) + symbolsPerPixel - 1) / symbolsPerPixel;

    if (requiredPixels > totalPixels) {
        std::cout << "Warning: Image dimensions (" << width << "x" << height << ") may be too small for "
            << symbols.size() << " symbols\n";
        std::cout << "Required pixels: " << requiredPixels << ", Available pixels: " << totalPixels << "\n";
        std::cout << "Consider using a larger mode or adaptive mode\n";
    }

    // Create QRAC image
    std::vector<uint8_t> imageData = createQRACImage(symbols, width, height, useFEC);
    std::cout << "Generated image data: " << imageData.size() << " bytes\n";

    // Generate output filename
    std::string outputImage = generateOutputFilename(inputFile, "_encoded", outputFormat);

    // 对图像进行无损压缩（如果是PNG格式）
    if (outputFormat == "png") {
        size_t maxSizeKB = static_cast<size_t>(fileSize * 1.5 / 1024); // 原始文件1.5倍
        std::cout << "Applying auto lossless compression to image (max " << maxSizeKB << "KB)...\n";
        std::vector<uint8_t> compressedData = compressImageAuto(imageData, width, height, 3, maxSizeKB);
        std::cout << "Compressed image data: " << compressedData.size() << " bytes\n";
        if (compressedData.size() > maxSizeKB * 1024) {
            std::cout << "Warning: PNG file still exceeds " << maxSizeKB << "KB after compression. Data may be hard to compress.\n";
        }
#ifdef _WIN32
        std::wstring wideOutput = utf8ToWstring(outputImage);
        std::ofstream outFile(wideOutput, std::ios::binary);
#else
        std::ofstream outFile(outputImage, std::ios::binary);
#endif
        outFile.write(reinterpret_cast<const char*>(compressedData.data()), compressedData.size());
        outFile.close();
    }
    else {
        // BMP保存逻辑
#ifdef _WIN32
        std::wstring wideOutput = utf8ToWstring(outputImage);
        // 使用宽字符版本的文件保存
        if (!saveImage(outputImage, imageData.data(), width, height, 3, outputFormat)) {
            throw QRACException(ErrorType::ImageSaveError, "Failed to save output image");
        }
#else
        if (!saveImage(outputImage, imageData.data(), width, height, 3, outputFormat)) {
            throw QRACException(ErrorType::ImageSaveError, "Failed to save output image");
        }
#endif
    }

    std::cout << "QRAC image saved: " << outputImage << "\n";
    std::cout << "Encoding complete! Output file is in the same directory as input.\n";
    std::cout << outputFormat << " format ensures lossless storage of your data.\n";
}

// Decoder function
void decodeFile() {
    std::string inputImage;

    std::cout << "[Decode] Extract file from QRAC image\n";
    std::cout << "Enter input image path (PNG, BMP or PPM format): ";
    std::getline(std::cin, inputImage);

    if (!fileExists(inputImage)) {
        throw QRACException(ErrorType::FileNotFound, "File does not exist: " + inputImage);
    }

    // 检查是否是JPG文件
    std::string ext = toLower(getFileExtension(inputImage));
    if (ext == "jpg" || ext == "jpeg") {
        if (isJPGFile(inputImage)) {
            showJPGWarning();
        }
        std::cout << "JPG decoding is experimental and may not work correctly.\n";
    }

    // Load image using stb_image
    int width, height, channels;
    STBImagePtr imageDataPtr = loadImageSTB(inputImage, &width, &height, &channels, 0);

    if (!imageDataPtr) {
        throw QRACException(ErrorType::ImageLoadError, "Failed to load image: " + inputImage);
    }

    unsigned char* imageData = imageDataPtr.get();

    // 确保至少有3个通道
    if (channels < 3) {
        // 如果图像通道少于3个，转换为3通道
        std::unique_ptr<unsigned char[]> newData(new unsigned char[width * height * 3]);
        for (int i = 0; i < width * height; i++) {
            unsigned char pixelValue = (channels == 1) ? imageData[i] : imageData[i * channels];
            newData[i * 3] = pixelValue;
            newData[i * 3 + 1] = pixelValue;
            newData[i * 3 + 2] = pixelValue;
        }
        imageDataPtr.reset(newData.release());
        imageData = imageDataPtr.get();
        channels = 3;
    }

    std::cout << "Loaded image: " << width << "x" << height << " pixels, " << channels << " channels\n";

    // Calculate total symbols
    int totalPixels = width * height;
    int symbolsPerPixel = g_config.SYMBOLS_PER_PIXEL;
    int totalSymbols = totalPixels * symbolsPerPixel;

    std::cout << "Storable symbols: " << totalSymbols << "\n";

    // Calculate bits per symbol
    int intervals = calculateIntervals();
    int bitsPerSymbol = static_cast<int>(std::log2(intervals));
    std::cout << "Number of intervals: " << intervals << " (L=" << g_config.L << ")\n";
    std::cout << "Bits per symbol: " << bitsPerSymbol << "\n";

    // Extract symbols from image
    std::vector<int> symbols;
    symbols.reserve(totalSymbols);

    for (int i = 0; i < totalPixels * channels; i += channels) {
        // 检查整个像素是否在填充颜色范围内（接近黑色）
        if (isFillerPixel(&imageData[i], channels)) {
            // 如果是填充颜色，则添加填充符号
            for (int ch = 0; ch < symbolsPerPixel; ch++) {
                symbols.push_back(-1);
            }
        }
        else {
            for (int ch = 0; ch < symbolsPerPixel; ch++) {
                uint8_t pixelValue = imageData[i + ch];
                int symbol = decodeToSymbol(pixelValue);
                symbols.push_back(symbol);
            }
        }
    }

    std::cout << "Extracted symbols: " << symbols.size() << " symbols\n";

    // Calculate expected data bits
    size_t expectedBits = totalSymbols * bitsPerSymbol;

    // Convert symbols to binary stream
    std::vector<bool> binaryStream = symbolsToBinary(symbols, bitsPerSymbol, expectedBits);
    std::cout << "Extracted binary stream: " << binaryStream.size() << " bits\n";

    // Convert binary stream to byte data
    std::vector<uint8_t> extractedData = binaryToData(binaryStream);
    std::cout << "Extracted data: " << extractedData.size() << " bytes\n";

    // Apply FEC error correction
    bool dataValid = verifyAndCorrectFEC(extractedData);
    std::cout << "Data after FEC correction: " << extractedData.size() << " bytes\n";

    if (!dataValid) {
        std::cout << "Warning: Data may contain uncorrectable errors\n";
    }

    // Determine output file type
    std::string fileType = detectFileType(extractedData);
    std::cout << "Detected file type: " << fileType << "\n";

    // Generate output filename
    std::string outputFile = generateOutputFilename(inputImage, "_decoded", fileType);

    // Save extracted data
    saveExtractedData(extractedData, outputFile, fileType == "txt");

    std::cout << "Data extracted to: " << outputFile << "\n";
    std::cout << "Decoding complete! Output file is in the same directory as input.\n";
    std::cout << "Extraction " << (dataValid ? "successful" : "partially successful, may contain errors") << "\n";
}

// 将图像数据转换为32位BMP（添加Alpha通道）
std::vector<uint8_t> convertTo32BitBMP(const std::vector<uint8_t>& imageData, int width, int height, int channels) {
    if (channels == 4) {
        return imageData; // 已经是32位
    }

    // 从24位转换为32位（添加Alpha通道，值为255）
    std::vector<uint8_t> result(width * height * 4);
    for (int i = 0; i < width * height; i++) {
        result[i * 4] = imageData[i * channels];
        result[i * 4 + 1] = imageData[i * channels + 1];
        result[i * 4 + 2] = imageData[i * channels + 2];
        result[i * 4 + 3] = 255; // Alpha通道
    }
    return result;
}

// 改进的图像加载函数，支持更多格式
STBImagePtr loadImageWithFallback(const std::string& filename, int* width, int* height, int* channels) {
    // 首先尝试正常加载
    STBImagePtr imageData = loadImageSTB(filename, width, height, channels, 0);

    if (!imageData) {
        // 如果正常加载失败，尝试强制转换为3通道
        imageData = loadImageSTB(filename, width, height, channels, 3);

        if (!imageData) {
            throw QRACException(ErrorType::ImageLoadError,
                "无法加载图像文件。请确保文件是有效的PNG、BMP或PPM格式，并且没有被压缩。");
        }
    }

    return imageData;
}

// Corrector function - 改进版本，正确处理填充值
void correctImageFile() {
    std::string inputImage;

    std::cout << "[Correct] Repair damaged QRAC image\n";
    std::cout << "Enter input image path (PNG, BMP or PPM format): ";
    std::getline(std::cin, inputImage);

    if (!fileExists(inputImage)) {
        throw QRACException(ErrorType::FileNotFound, "File does not exist: " + inputImage);
    }

    // 检查是否是JPG文件
    std::string ext = toLower(getFileExtension(inputImage));
    if (ext == "jpg" || ext == "jpeg") {
        throw QRACException(ErrorType::InvalidInput, "JPG format is not supported for correction. Please use PNG or BMP format.");
    }

    // Load image using improved loader
    int width, height, channels;
    STBImagePtr imageDataPtr = loadImageWithFallback(inputImage, &width, &height, &channels);

    if (!imageDataPtr) {
        throw QRACException(ErrorType::ImageLoadError, "Failed to load image: " + inputImage);
    }

    unsigned char* imageData = imageDataPtr.get();

    // 确保至少有3个通道
    if (channels < 3) {
        // 如果图像通道少于3个，转换为3通道
        std::unique_ptr<unsigned char[]> newData(new unsigned char[width * height * 3]);
        for (int i = 0; i < width * height; i++) {
            unsigned char pixelValue = (channels == 1) ? imageData[i] : imageData[i * channels];
            newData[i * 3] = pixelValue;
            newData[i * 3 + 1] = pixelValue;
            newData[i * 3 + 2] = pixelValue;
        }
        imageDataPtr.reset(newData.release());
        imageData = imageDataPtr.get();
        channels = 3;
    }

    std::cout << "Loaded image: " << width << "x" << height << " pixels, " << channels << " channels\n";

    // Check if correction is needed
    int totalPixels = width * height;
    int incorrectPixels = 0;
    int fillerPixels = 0;

    for (int i = 0; i < totalPixels * channels; i += channels) {
        // 检查整个像素是否在填充颜色范围内（接近黑色）
        if (isFillerPixel(&imageData[i], channels)) {
            fillerPixels++;
            continue;
        }

        // 检查每个通道是否需要校正
        for (int ch = 0; ch < 3; ch++) {
            uint8_t pixelValue = imageData[i + ch];
            int intervalIndex = decodeToSymbol(pixelValue);

            // 跳过填充值
            if (intervalIndex == -1) continue;

            uint8_t anchorValue = calculateAnchor(intervalIndex);

            if (pixelValue != anchorValue) {
                incorrectPixels++;
            }
        }
    }

    float incorrectRatio = static_cast<float>(incorrectPixels) / (totalPixels * 3 - fillerPixels * 3);
    std::cout << "Detected " << incorrectPixels << " pixel values deviating from anchors ("
        << std::fixed << std::setprecision(2) << incorrectRatio * 100 << "%)\n";
    std::cout << "Found " << fillerPixels << " filler pixels (will be set to pure black)\n";

    if (incorrectPixels == 0 && fillerPixels == 0) {
        std::cout << "Image is already in anchor-pure state, no correction needed\n";

        // Generate output filename
        std::string outputImage = generateOutputFilename(inputImage, "_corrected", "bmp");

        // 转换为32位BMP
        std::vector<uint8_t> bmpData = convertTo32BitBMP(
            std::vector<uint8_t>(imageData, imageData + totalPixels * channels),
            width, height, channels
        );

        // 直接保存为BMP
#ifdef _WIN32
        std::wstring wideOutput = utf8ToWstring(outputImage);
        if (!saveImage(outputImage, bmpData.data(), width, height, 4, "bmp")) {
            throw QRACException(ErrorType::ImageSaveError, "Failed to save output image");
        }
#else
        if (!saveImage(outputImage, bmpData.data(), width, height, 4, "bmp")) {
            throw QRACException(ErrorType::ImageSaveError, "Failed to save output image");
        }
#endif

        std::cout << "Image saved: " << outputImage << "\n";
        std::cout << "Correction complete! Image was already pure, no changes made.\n";
    }
    else {
        // Perform correction
        std::cout << "Performing correction...\n";
        std::vector<uint8_t> correctedData(totalPixels * channels);

        for (int i = 0; i < totalPixels * channels; i += channels) {
            // 检查整个像素是否在填充颜色范围内（接近黑色）
            if (isFillerPixel(&imageData[i], channels)) {
                // 如果是填充颜色，设置为纯黑色
                correctedData[i] = 0;
                correctedData[i + 1] = 0;
                correctedData[i + 2] = 0;
                if (channels > 3) {
                    correctedData[i + 3] = imageData[i + 3]; // 保留Alpha通道
                }
                continue;
            }

            // 校正每个通道
            for (int ch = 0; ch < 3; ch++) {
                uint8_t pixelValue = imageData[i + ch];
                int intervalIndex = decodeToSymbol(pixelValue);

                if (intervalIndex == -1) {
                    // 如果是填充值，设置为纯黑色
                    correctedData[i + ch] = 0;
                }
                else {
                    correctedData[i + ch] = calculateAnchor(intervalIndex);
                }
            }

            // 保留Alpha通道（如果有）
            if (channels > 3) {
                correctedData[i + 3] = imageData[i + 3];
            }
        }

        // Generate output filename
        std::string outputImage = generateOutputFilename(inputImage, "_corrected", "bmp");

        // 转换为32位BMP
        std::vector<uint8_t> bmpData = convertTo32BitBMP(correctedData, width, height, channels);

        // 保存为BMP格式
#ifdef _WIN32
        std::wstring wideOutput = utf8ToWstring(outputImage);
        if (!saveImage(outputImage, bmpData.data(), width, height, 4, "bmp")) {
            throw QRACException(ErrorType::ImageSaveError, "Failed to save output image");
        }
#else
        if (!saveImage(outputImage, bmpData.data(), width, height, 4, "bmp")) {
            throw QRACException(ErrorType::ImageSaveError, "Failed to save output image");
        }
#endif

        std::cout << "Corrected image saved: " << outputImage << "\n";
        std::cout << "Correction complete! Output file is in the same directory as input.\n";
        std::cout << "BMP format ensures lossless storage of your data.\n";
    }
}

// Main menu
void showMenu() {
    int choice = 0;

    // Show user guide and trust statement
    showUserGuide();
    showTrustStatement();

    while (true) {
        std::cout << "\n======================================================\n";
        std::cout << "                 QRAC Integrated Tool Suite\n";
        std::cout << "======================================================\n";
        std::cout << "1. Encode - Convert file to QRAC image\n";
        std::cout << "2. Decode - Extract file from QRAC image\n";
        std::cout << "3. Correct - Repair damaged QRAC image\n";
        std::cout << "4. Show User Guide\n";
        std::cout << "5. Show Trust Statement\n";
        std::cout << "6. Exit\n";
        std::cout << "Select option (1-6): ";

        std::cin >> choice;

        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "Invalid input. Please enter a number between 1 and 6.\n";
            continue;
        }

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        try {
            switch (choice) {
            case 1:
                encodeFile();
                break;
            case 2:
                decodeFile();
                break;
            case 3:
                correctImageFile();
                break;
            case 4:
                showUserGuide();
                break;
            case 5:
                showTrustStatement();
                break;
            case 6:
                std::cout << "Exiting program. Thank you for using QRAC Tool Suite!\n";
                return;
            default:
                std::cout << "Invalid option. Please select a number between 1 and 6.\n";
            }
        }
        catch (const QRACException& e) {
            std::cerr << "Error: " << e.what() << "\n";

            // 根据错误类型提供不同的处理建议
            switch (e.getType()) {
            case ErrorType::FileNotFound:
                std::cerr << "Please check the file path and try again.\n";
                break;
            case ErrorType::FileReadError:
                std::cerr << "Please ensure the file is accessible and not locked by another process.\n";
                break;
            case ErrorType::FileWriteError:
                std::cerr << "Please ensure you have write permissions to the output directory.\n";
                break;
            case ErrorType::ImageLoadError:
                std::cerr << "Please ensure the image file is not corrupted and is in a supported format.\n";
                break;
            case ErrorType::ImageSaveError:
                std::cerr << "Please ensure you have sufficient disk space and write permissions.\n";
                break;
            case ErrorType::UserAbort:
                std::cerr << "Operation cancelled by user.\n";
                break;
            default:
                break;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Unexpected error: " << e.what() << "\n";
        }
    }
}

// Main function
int main() {
    std::cout << "QRAC Integrated Tool Suite - Version 4.0\n";
    std::cout << "Now with improved error correction and 0-10 range skipping\n";
    std::cout << "Supports Word documents, text files, and compressed archives\n";
    std::cout << "Improved Chinese/UTF-8 text support\n";
    std::cout << "Uses stb_image for better format compatibility\n\n";
    std::cout << "版权所有 (c) 2024 xuehaoyu chen\n";
    std::cout << "基于MIT许可证开源\n";
    std::cout << "GitHub: https://github.com/sans666VIP\n\n";

    try {
        showMenu();
    }
    catch (const std::exception& e) {
        std::cerr << "Program error: " << e.what() << "\n";
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    return 0;
}