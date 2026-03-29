/*
 * MCP (Model Context Protocol) server for FS-UAE.
 * Provides JSON-RPC 2.0 introspection via Unix domain socket or TCP.
 *
 * Supports:
 *   /tmp/fs-uae-mcp.sock       Unix socket
 *   tcp:6789                    TCP on localhost
 *   tcp:0.0.0.0:6789            TCP on all interfaces
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "uae/uae.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "debug.h"
#include "mcp_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <poll.h>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

#include <png.h>

// external video state
extern "C" {
    extern RenderData g_renderdata;
    extern int g_amiga_video_bpp;
    extern int g_amiga_video_format;
}

// Base64 encoding
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char* data, size_t len)
{
    std::string result;
    result.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        unsigned int n = (unsigned int)data[i] << 16;
        if (i + 1 < len) n |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) n |= (unsigned int)data[i + 2];
        result += b64_table[(n >> 18) & 0x3f];
        result += b64_table[(n >> 12) & 0x3f];
        result += (i + 1 < len) ? b64_table[(n >> 6) & 0x3f] : '=';
        result += (i + 2 < len) ? b64_table[n & 0x3f] : '=';
    }
    return result;
}

// JSON helpers
static std::string jsonrpc_result(const std::string& id, const std::string& resultJson)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + resultJson + "}\n";
}

static std::string jsonrpc_error(const std::string& id, int code, const std::string& message)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + message + "\"}}\n";
}

static std::string json_escape(const std::string& s)
{
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                result += buf;
            }
            else
            {
                result += c;
            }
            break;
        }
    }
    return result;
}

static std::string to_hex(uint32_t value, int digits = 0)
{
    char buf[32];
    if (digits > 0)
        snprintf(buf, sizeof(buf), "%0*x", digits, value);
    else
        snprintf(buf, sizeof(buf), "%x", value);
    return buf;
}

// Simple JSON value extraction (no external dependency)
static std::string json_get_string(const std::string& json, const char* key)
{
    std::string search = std::string("\"") + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    pos++;
    std::string result;
    while (pos < json.size() && json[pos] != '"')
    {
        if (json[pos] == '\\' && pos + 1 < json.size())
        {
            pos++;
            switch (json[pos])
            {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: result += json[pos]; break;
            }
        }
        else
        {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

static int64_t json_get_int(const std::string& json, const char* key, int64_t defaultVal = 0)
{
    std::string search = std::string("\"") + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return defaultVal;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return defaultVal;
    // Handle hex with 0x prefix
    if (pos + 1 < json.size() && json[pos] == '0' && (json[pos + 1] == 'x' || json[pos + 1] == 'X'))
        return strtoll(json.c_str() + pos, nullptr, 16);
    // Handle "0x..." as a string value
    if (json[pos] == '"')
    {
        pos++;
        return strtoll(json.c_str() + pos, nullptr, 0);
    }
    return strtoll(json.c_str() + pos, nullptr, 10);
}

static bool json_has_key(const std::string& json, const char* key)
{
    std::string search = std::string("\"") + key + "\"";
    return json.find(search) != std::string::npos;
}

// PNG encoding to memory buffer
struct PngBuffer {
    std::vector<unsigned char> data;
};

static void png_write_to_buffer(png_structp png, png_bytep data, png_size_t length)
{
    PngBuffer* buf = (PngBuffer*)png_get_io_ptr(png);
    buf->data.insert(buf->data.end(), data, data + length);
}

static void png_flush_buffer(png_structp png)
{
    (void)png;
}

// MCP Server state
static std::thread g_serverThread;
static std::atomic<bool> g_running{false};
static int g_acceptorFd = -1;
static std::atomic<int> g_clientFd{-1};
static std::string g_socketPath;

enum class TransportType { UNIX_SOCKET, TCP };
static TransportType g_transport;

// Tool implementations
static std::string tool_machine_info()
{
    std::string json = "{\"emulator\":\"fs-uae\"";
    json += ",\"model\":\"" + json_escape(std::string(currprefs.description)) + "\"";
    json += ",\"cpu_model\":" + std::to_string(currprefs.cpu_model);
    json += ",\"chip_memory\":" + std::to_string(currprefs.chipmem_size);
    json += ",\"fast_memory\":" + std::to_string(currprefs.fastmem_size);
    json += ",\"bogomem\":" + std::to_string(currprefs.bogomem_size);
    json += "}";
    return json;
}

static std::string tool_cpu_registers()
{
    std::string json = "{";
    for (int i = 0; i < 8; i++)
    {
        if (i > 0) json += ",";
        json += "\"D" + std::to_string(i) + "\":\"" + to_hex(m68k_dreg(regs, i), 8) + "\"";
    }
    for (int i = 0; i < 8; i++)
    {
        json += ",\"A" + std::to_string(i) + "\":\"" + to_hex(m68k_areg(regs, i), 8) + "\"";
    }
    json += ",\"PC\":\"" + to_hex(m68k_getpc(), 8) + "\"";
    json += ",\"SR\":\"" + to_hex(regs.sr, 4) + "\"";
    json += ",\"USP\":\"" + to_hex(regs.usp, 8) + "\"";
    json += ",\"ISP\":\"" + to_hex(regs.isp, 8) + "\"";
    json += ",\"MSP\":\"" + to_hex(regs.msp, 8) + "\"";
    json += ",\"VBR\":\"" + to_hex(regs.vbr, 8) + "\"";
    json += "}";
    return json;
}

static std::string tool_memory_read(const std::string& params)
{
    int64_t addr = json_get_int(params, "address");
    int64_t length = json_get_int(params, "length", 256);
    if (length > 65536) length = 65536;
    if (length <= 0) return "{\"error\":\"invalid length\"}";

    std::string hex;
    hex.reserve(length * 2);
    for (int64_t i = 0; i < length; i++)
    {
        uae_u8 val = get_byte_debug((uaecptr)(addr + i));
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x", val);
        hex += buf;
    }

    std::string json = "{\"address\":\"" + to_hex((uint32_t)addr, 8) + "\"";
    json += ",\"length\":" + std::to_string(length);
    json += ",\"data\":\"" + hex + "\"";

    // Also provide ASCII representation
    std::string ascii;
    for (int64_t i = 0; i < length; i++)
    {
        uae_u8 val = get_byte_debug((uaecptr)(addr + i));
        ascii += (val >= 0x20 && val < 0x7f) ? (char)val : '.';
    }
    json += ",\"ascii\":\"" + json_escape(ascii) + "\"";
    json += "}";
    return json;
}

static std::string tool_memory_write(const std::string& params)
{
    int64_t addr = json_get_int(params, "address");
    std::string data = json_get_string(params, "data");

    if (data.size() % 2 != 0)
        return "{\"error\":\"data must be even number of hex characters\"}";

    int count = 0;
    for (size_t i = 0; i < data.size(); i += 2)
    {
        char hi = data[i], lo = data[i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0)
            return "{\"error\":\"invalid hex character\"}";
        debug_write_memory_8((uaecptr)(addr + count), (uae_u8)((h << 4) | l));
        count++;
    }

    return "{\"address\":\"" + to_hex((uint32_t)addr, 8) + "\",\"bytes_written\":" + std::to_string(count) + "}";
}

static std::string tool_disassemble(const std::string& params)
{
    int64_t addr = json_get_int(params, "address");
    int64_t count = json_get_int(params, "count", 10);
    if (count <= 0) count = 1;
    if (count > 100) count = 100;

    // Use debug_parser with the 'd' (disassemble) command
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "d %x %d", (unsigned int)addr, (int)count);

    char outbuf[16384];
    memset(outbuf, 0, sizeof(outbuf));
    debug_parser(cmd, outbuf, sizeof(outbuf) - 1);

    return "{\"address\":\"" + to_hex((uint32_t)addr, 8) + "\",\"output\":\"" + json_escape(outbuf) + "\"}";
}

static std::string tool_debug_command(const std::string& params)
{
    std::string cmd = json_get_string(params, "command");
    if (cmd.empty())
        return "{\"error\":\"missing command parameter\"}";

    char outbuf[65536];
    memset(outbuf, 0, sizeof(outbuf));
    debug_parser(cmd.c_str(), outbuf, sizeof(outbuf) - 1);

    return "{\"output\":\"" + json_escape(outbuf) + "\"}";
}

static std::string tool_debugger_break()
{
    activate_debugger();
    return "{\"status\":\"break requested\"}";
}

static std::string tool_debugger_run()
{
    deactivate_debugger();
    return "{\"status\":\"run requested\"}";
}

static std::string tool_screenshot()
{
    if (!g_renderdata.pixels)
        return "{\"error\":\"no framebuffer available\"}";

    int width = g_renderdata.limit_w;
    int height = g_renderdata.limit_h;
    int srcX = g_renderdata.limit_x;
    int srcY = g_renderdata.limit_y;
    int bpp = g_renderdata.bpp;
    int stride = g_renderdata.width * bpp;

    if (width <= 0 || height <= 0)
        return "{\"error\":\"invalid dimensions\"}";

    // Create PNG in memory
    PngBuffer pngBuf;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        return "{\"error\":\"png_create_write_struct failed\"}";

    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_write_struct(&png, NULL);
        return "{\"error\":\"png_create_info_struct failed\"}";
    }

    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        return "{\"error\":\"PNG encoding error\"}";
    }

    png_set_write_fn(png, &pngBuf, png_write_to_buffer, png_flush_buffer);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    // Convert pixels to RGB
    std::vector<unsigned char> row(width * 3);
    for (int y = 0; y < height; y++)
    {
        unsigned char* src = g_renderdata.pixels + (srcY + y) * stride + srcX * bpp;
        for (int x = 0; x < width; x++)
        {
            if (bpp == 4)
            {
                if (g_amiga_video_format == AMIGA_VIDEO_FORMAT_RGBA)
                {
                    row[x * 3 + 0] = src[x * 4 + 0]; // R
                    row[x * 3 + 1] = src[x * 4 + 1]; // G
                    row[x * 3 + 2] = src[x * 4 + 2]; // B
                }
                else
                {
                    // BGRA
                    row[x * 3 + 0] = src[x * 4 + 2]; // R
                    row[x * 3 + 1] = src[x * 4 + 1]; // G
                    row[x * 3 + 2] = src[x * 4 + 0]; // B
                }
            }
            else if (bpp == 2)
            {
                // R5G6B5
                uint16_t pixel = src[x * 2] | (src[x * 2 + 1] << 8);
                row[x * 3 + 0] = (pixel >> 11) << 3;
                row[x * 3 + 1] = ((pixel >> 5) & 0x3f) << 2;
                row[x * 3 + 2] = (pixel & 0x1f) << 3;
            }
        }
        png_write_row(png, row.data());
    }

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);

    std::string b64 = base64_encode(pngBuf.data.data(), pngBuf.data.size());

    return "{\"width\":" + std::to_string(width) +
           ",\"height\":" + std::to_string(height) +
           ",\"format\":\"png\"" +
           ",\"data\":\"" + b64 + "\"}";
}

static std::string tool_memory_search(const std::string& params)
{
    int64_t startAddr = json_get_int(params, "start", 0);
    int64_t endAddr = json_get_int(params, "end", 0x1000000);
    std::string pattern = json_get_string(params, "pattern");
    std::string text = json_get_string(params, "text");
    int64_t maxResults = json_get_int(params, "max_results", 10);

    std::vector<uint8_t> searchBytes;
    if (!text.empty())
    {
        for (char c : text)
            searchBytes.push_back((uint8_t)c);
    }
    else if (!pattern.empty())
    {
        for (size_t i = 0; i + 1 < pattern.size(); i += 2)
        {
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int h = nibble(pattern[i]), l = nibble(pattern[i + 1]);
            if (h < 0 || l < 0) return "{\"error\":\"invalid hex pattern\"}";
            searchBytes.push_back((uint8_t)((h << 4) | l));
        }
    }
    else
    {
        return "{\"error\":\"provide 'text' or 'pattern' parameter\"}";
    }

    if (searchBytes.empty())
        return "{\"error\":\"empty search pattern\"}";

    std::string json = "{\"results\":[";
    int found = 0;
    for (int64_t addr = startAddr; addr <= endAddr - (int64_t)searchBytes.size(); addr++)
    {
        bool match = true;
        for (size_t j = 0; j < searchBytes.size(); j++)
        {
            if (get_byte_debug((uaecptr)(addr + j)) != searchBytes[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            if (found > 0) json += ",";
            json += "\"" + to_hex((uint32_t)addr, 8) + "\"";
            found++;
            if (found >= maxResults) break;
        }
    }
    json += "],\"count\":" + std::to_string(found) + "}";
    return json;
}

// Tool dispatch
static std::string handle_tools_call(const std::string& id, const std::string& toolName, const std::string& args)
{
    try
    {
        std::string result;
        if (toolName == "machine_info")
            result = tool_machine_info();
        else if (toolName == "cpu_registers")
            result = tool_cpu_registers();
        else if (toolName == "memory_read")
            result = tool_memory_read(args);
        else if (toolName == "memory_write")
            result = tool_memory_write(args);
        else if (toolName == "disassemble")
            result = tool_disassemble(args);
        else if (toolName == "debug_command")
            result = tool_debug_command(args);
        else if (toolName == "debugger_break")
            result = tool_debugger_break();
        else if (toolName == "debugger_run")
            result = tool_debugger_run();
        else if (toolName == "screenshot")
            result = tool_screenshot();
        else if (toolName == "memory_search")
            result = tool_memory_search(args);
        else
            return jsonrpc_error(id, -32602, "Unknown tool: " + toolName);

        // Wrap in MCP content format
        return jsonrpc_result(id, "{\"content\":[{\"type\":\"text\",\"text\":" +
            std::string("\"") + json_escape(result) + "\"}]}");
    }
    catch (std::exception& e)
    {
        return jsonrpc_error(id, -32603, std::string("Tool error: ") + e.what());
    }
}

static std::string handle_initialize(const std::string& id)
{
    return jsonrpc_result(id,
        "{"
            "\"protocolVersion\":\"2024-11-05\","
            "\"capabilities\":{\"tools\":{}},"
            "\"serverInfo\":{"
                "\"name\":\"fs-uae-mcp\","
                "\"version\":\"1.0.0\""
            "}"
        "}");
}

static std::string handle_tools_list(const std::string& id)
{
    return jsonrpc_result(id,
        "{\"tools\":["
            "{"
                "\"name\":\"machine_info\","
                "\"description\":\"Get information about the emulated Amiga\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
            "},"
            "{"
                "\"name\":\"cpu_registers\","
                "\"description\":\"Get 68k CPU register values (D0-D7, A0-A7, PC, SR, etc)\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
            "},"
            "{"
                "\"name\":\"memory_read\","
                "\"description\":\"Read bytes from emulated memory. Returns hex and ASCII.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                    "\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
                    "\"length\":{\"type\":\"integer\",\"description\":\"Number of bytes (max 65536, default 256)\"}"
                "},\"required\":[\"address\"]}"
            "},"
            "{"
                "\"name\":\"memory_write\","
                "\"description\":\"Write hex bytes to emulated memory.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                    "\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
                    "\"data\":{\"type\":\"string\",\"description\":\"Hex string of bytes to write\"}"
                "},\"required\":[\"address\",\"data\"]}"
            "},"
            "{"
                "\"name\":\"disassemble\","
                "\"description\":\"Disassemble 68k instructions at an address.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                    "\"address\":{\"type\":\"integer\",\"description\":\"Start address\"},"
                    "\"count\":{\"type\":\"integer\",\"description\":\"Number of instructions (default 10)\"}"
                "},\"required\":[\"address\"]}"
            "},"
            "{"
                "\"name\":\"debug_command\","
                "\"description\":\"Execute an FS-UAE debugger command and return output. Supports all built-in debugger commands like 'm' (memory dump), 'd' (disassemble), 'f' (breakpoint), etc.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                    "\"command\":{\"type\":\"string\",\"description\":\"Debugger command string\"}"
                "},\"required\":[\"command\"]}"
            "},"
            "{"
                "\"name\":\"debugger_break\","
                "\"description\":\"Break execution (pause the emulated Amiga)\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
            "},"
            "{"
                "\"name\":\"debugger_run\","
                "\"description\":\"Resume execution (continue running)\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
            "},"
            "{"
                "\"name\":\"screenshot\","
                "\"description\":\"Take a screenshot of the emulated screen. Returns base64-encoded PNG.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
            "},"
            "{"
                "\"name\":\"memory_search\","
                "\"description\":\"Search memory for a byte pattern or text string.\","
                "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                    "\"start\":{\"type\":\"integer\",\"description\":\"Start address (default 0)\"},"
                    "\"end\":{\"type\":\"integer\",\"description\":\"End address (default 0x1000000)\"},"
                    "\"pattern\":{\"type\":\"string\",\"description\":\"Hex byte pattern to search for\"},"
                    "\"text\":{\"type\":\"string\",\"description\":\"Text string to search for\"},"
                    "\"max_results\":{\"type\":\"integer\",\"description\":\"Max results (default 10)\"}"
                "}}"
            "}"
        "]}");
}

static std::string handle_request(const std::string& line)
{
    // Minimal JSON-RPC parsing without external library
    // Extract method
    std::string method = json_get_string(line, "method");

    // Extract id
    std::string id = "null";
    size_t idPos = line.find("\"id\"");
    if (idPos != std::string::npos)
    {
        size_t colonPos = line.find(':', idPos + 4);
        if (colonPos != std::string::npos)
        {
            size_t valStart = colonPos + 1;
            while (valStart < line.size() && line[valStart] == ' ') valStart++;
            if (valStart < line.size())
            {
                if (line[valStart] == '"')
                {
                    size_t valEnd = line.find('"', valStart + 1);
                    if (valEnd != std::string::npos)
                        id = line.substr(valStart, valEnd - valStart + 1);
                }
                else
                {
                    size_t valEnd = valStart;
                    while (valEnd < line.size() && (isdigit(line[valEnd]) || line[valEnd] == '-'))
                        valEnd++;
                    id = line.substr(valStart, valEnd - valStart);
                }
            }
        }
    }

    if (method.empty())
        return jsonrpc_error("null", -32600, "Invalid Request");

    if (method == "notifications/initialized")
        return "";

    if (method == "initialize")
        return handle_initialize(id);

    if (method == "tools/list")
        return handle_tools_list(id);

    if (method == "tools/call")
    {
        // Extract tool name from params.name
        // Find "params" object, then "name" within it
        size_t paramsPos = line.find("\"params\"");
        if (paramsPos == std::string::npos)
            return jsonrpc_error(id, -32602, "Missing params");

        // Extract the params section
        size_t braceStart = line.find('{', paramsPos);
        if (braceStart == std::string::npos)
            return jsonrpc_error(id, -32602, "Invalid params");

        std::string paramsSection = line.substr(braceStart);
        std::string toolName = json_get_string(paramsSection, "name");
        if (toolName.empty())
            return jsonrpc_error(id, -32602, "Missing tool name");

        // Extract arguments section
        std::string argsJson = "{}";
        size_t argsPos = paramsSection.find("\"arguments\"");
        if (argsPos != std::string::npos)
        {
            size_t argsBrace = paramsSection.find('{', argsPos);
            if (argsBrace != std::string::npos)
            {
                // Find matching closing brace
                int depth = 0;
                size_t end = argsBrace;
                for (; end < paramsSection.size(); end++)
                {
                    if (paramsSection[end] == '{') depth++;
                    else if (paramsSection[end] == '}') { depth--; if (depth == 0) break; }
                }
                argsJson = paramsSection.substr(argsBrace, end - argsBrace + 1);
            }
        }

        return handle_tools_call(id, toolName, argsJson);
    }

    return jsonrpc_error(id, -32601, "Method not found: " + method);
}

// Socket infrastructure
static bool setup_unix_socket(const std::string& path)
{
    ::unlink(path.c_str());
    g_acceptorFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_acceptorFd < 0)
    {
        write_log("MCP: failed to create unix socket: %s\n", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(g_acceptorFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        write_log("MCP: failed to bind unix socket '%s': %s\n", path.c_str(), strerror(errno));
        ::close(g_acceptorFd);
        g_acceptorFd = -1;
        return false;
    }

    if (::listen(g_acceptorFd, 1) < 0)
    {
        write_log("MCP: listen failed: %s\n", strerror(errno));
        ::close(g_acceptorFd);
        g_acceptorFd = -1;
        return false;
    }

    return true;
}

static bool setup_tcp_socket(const std::string& host, int port)
{
    g_acceptorFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_acceptorFd < 0)
    {
        write_log("MCP: failed to create tcp socket: %s\n", strerror(errno));
        return false;
    }

    int optval = 1;
    ::setsockopt(g_acceptorFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
    {
        write_log("MCP: invalid address '%s'\n", host.c_str());
        ::close(g_acceptorFd);
        g_acceptorFd = -1;
        return false;
    }

    if (::bind(g_acceptorFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        write_log("MCP: failed to bind tcp %s:%d: %s\n", host.c_str(), port, strerror(errno));
        ::close(g_acceptorFd);
        g_acceptorFd = -1;
        return false;
    }

    if (::listen(g_acceptorFd, 1) < 0)
    {
        write_log("MCP: listen failed: %s\n", strerror(errno));
        ::close(g_acceptorFd);
        g_acceptorFd = -1;
        return false;
    }

    return true;
}

static void handle_connection(int clientFd)
{
    g_clientFd.store(clientFd);
    std::string buffer;
    char readBuf[4096];

    while (g_running)
    {
        struct pollfd pfd = { clientFd, POLLIN, 0 };
        int ret = ::poll(&pfd, 1, 500);
        if (ret < 0) break;
        if (ret == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        ssize_t n = ::read(clientFd, readBuf, sizeof(readBuf));
        if (n <= 0) break;

        buffer.append(readBuf, n);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty()) continue;

            std::string response = handle_request(line);
            if (!response.empty())
            {
                const char* data = response.c_str();
                size_t remaining = response.size();
                while (remaining > 0)
                {
                    ssize_t written = ::write(clientFd, data, remaining);
                    if (written <= 0) return;
                    data += written;
                    remaining -= written;
                }
            }
        }
    }
}

static void accept_loop()
{
    while (g_running && g_acceptorFd >= 0)
    {
        struct pollfd pfd = { g_acceptorFd, POLLIN, 0 };
        int ret = ::poll(&pfd, 1, 500);
        if (ret < 0 || !g_running) break;
        if (ret == 0) continue;

        int clientFd = ::accept(g_acceptorFd, nullptr, nullptr);
        if (clientFd < 0)
        {
            if (g_running)
                write_log("MCP: accept failed: %s\n", strerror(errno));
            break;
        }

        write_log("MCP: client connected\n");
        handle_connection(clientFd);
        g_clientFd.store(-1);
        ::close(clientFd);
        write_log("MCP: client disconnected\n");
    }
}

// Public API
extern "C" void mcp_start(const char* endpoint)
{
    std::string ep(endpoint);
    bool ok = false;

    if (ep.substr(0, 4) == "tcp:")
    {
        g_transport = TransportType::TCP;
        std::string rest = ep.substr(4);
        std::string host = "127.0.0.1";
        int port = 0;
        size_t lastColon = rest.rfind(':');
        if (lastColon != std::string::npos)
        {
            host = rest.substr(0, lastColon);
            port = std::stoi(rest.substr(lastColon + 1));
        }
        else
        {
            port = std::stoi(rest);
        }
        ok = setup_tcp_socket(host, port);
    }
    else
    {
        g_transport = TransportType::UNIX_SOCKET;
        g_socketPath = ep;
        ok = setup_unix_socket(ep);
    }

    if (ok)
    {
        g_running = true;
        write_log("MCP: listening on %s\n", ep.c_str());
        g_serverThread = std::thread(accept_loop);
    }
    else
    {
        write_log("MCP: failed to start on %s\n", ep.c_str());
    }
}

extern "C" void mcp_stop(void)
{
    g_running = false;

    int fd = g_clientFd.load();
    if (fd >= 0)
        ::shutdown(fd, SHUT_RDWR);

    if (g_acceptorFd >= 0)
    {
        ::shutdown(g_acceptorFd, SHUT_RDWR);
        ::close(g_acceptorFd);
        g_acceptorFd = -1;
    }

    if (g_serverThread.joinable())
        g_serverThread.join();

    if (g_transport == TransportType::UNIX_SOCKET && !g_socketPath.empty())
        ::unlink(g_socketPath.c_str());
}
