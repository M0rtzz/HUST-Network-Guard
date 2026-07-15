#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <curl/urlapi.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>

#include "resources/resource.h"
#endif

namespace
{

    constexpr char CONNECTIVITY_CHECK_URL[] =
        "https://www.baidu.com";
    constexpr char PORTAL_DISCOVERY_URL[] =
        "http://www.baidu.com";
    constexpr char PASSWORD_RSA_MODULUS[] =
        "94dd2a8675fb779e6b9f7103698634cd400f27a154afa67af6166a43fc26417"
        "222a79506d34cacc7641946abda1785b7acf9910ad6a0978c91ec84d40b71d"
        "2891379af19ffb333e7517e390bd26ac312fe940c340466b4a5d4af1d65c3b"
        "5944078f96a1a51a5a53e4bc302818b7c9f63c4a1b07bd7d874cef1c3d4b2"
        "f5eb7871";

    constexpr std::chrono::seconds DEFAULT_CHECK_INTERVAL{30};
    constexpr std::chrono::seconds FAILURE_RECHECK_INTERVAL{5};
    constexpr std::chrono::seconds LOGIN_VERIFY_DELAY{5};
    constexpr int DEFAULT_FAILURE_THRESHOLD = 3;
    constexpr std::array<std::chrono::seconds, 3> RETRY_DELAYS{
        std::chrono::seconds{10},
        std::chrono::seconds{30},
        std::chrono::seconds{60},
    };

    std::atomic<bool> monitor_running{true};
    std::atomic<bool> immediate_check_report_pending{false};
    std::atomic<bool> login_test_requested{false};
    std::condition_variable monitor_wait_condition;
    std::mutex monitor_wait_mutex;
    std::mutex log_mutex;
    bool immediate_check_requested = false;

    bool waitOrStop(std::chrono::seconds duration)
    {
        std::unique_lock<std::mutex> lock(monitor_wait_mutex);
        monitor_wait_condition.wait_for(lock, duration, []
                                        { return !monitor_running.load() || immediate_check_requested || login_test_requested.load(); });

        if (!monitor_running.load())
        {
            return false;
        }

        immediate_check_requested = false;
        return true;
    }

    void requestImmediateCheck()
    {
        immediate_check_report_pending.store(true);
        {
            std::lock_guard<std::mutex> lock(monitor_wait_mutex);
            immediate_check_requested = true;
        }
        monitor_wait_condition.notify_all();
    }

    void requestLoginConfigurationTest()
    {
        login_test_requested.store(true);
        monitor_wait_condition.notify_all();
    }

    void stopMonitor()
    {
        monitor_running.store(false);
        monitor_wait_condition.notify_all();
    }

    struct AppConfig
    {
        std::string user_id;
        std::string plain_password;
        std::chrono::seconds check_interval{DEFAULT_CHECK_INTERVAL};
        int failure_threshold = DEFAULT_FAILURE_THRESHOLD;
    };

    struct PortalParameters
    {
        std::string login_url;
        std::string query_string;
        std::string mac;
    };

    enum class LoginAttemptStatus
    {
        accepted,
        rejected,
        already_online,
        network_error,
        http_error,
        preparation_error,
        unknown_response,
    };

    struct LoginAttemptResult
    {
        LoginAttemptStatus status = LoginAttemptStatus::unknown_response;
        long http_code = 0;
        std::string server_result;
        std::string server_message;
        std::string detail;
    };

    void notifyConnected();
    void notifyDisconnected();
    void notifyConfigurationError();
    void notifyLoginTestResult(LoginAttemptStatus status);

    size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        const size_t byte_count = size * nmemb;
        static_cast<std::string *>(userp)->append(
            static_cast<char *>(contents), byte_count);
        return byte_count;
    }

    std::string executableDirectory()
    {
#ifdef _WIN32
        char executable_path[MAX_PATH];
        const DWORD path_length =
            GetModuleFileNameA(nullptr, executable_path, MAX_PATH);
        if (path_length > 0 && path_length < MAX_PATH)
        {
            std::string path(executable_path, path_length);
            const std::string::size_type separator = path.find_last_of("\\/");
            if (separator != std::string::npos)
            {
                return path.substr(0, separator + 1);
            }
        }
#endif
        return "";
    }

    std::string appFilePath(const std::string &file_name)
    {
        return executableDirectory() + file_name;
    }

    std::string logFilePath()
    {
        return appFilePath("HUST-Network-Guard.log");
    }

    std::string currentTime()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif

        std::ostringstream output;
        output << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
        return output.str();
    }

    void logMessage(const std::string &message)
    {
        static std::ofstream log_file(logFilePath(), std::ios::app);
        const std::lock_guard<std::mutex> lock(log_mutex);
        const std::string line = "[" + currentTime() + "] " + message;

        std::cout << line << std::endl;
        if (log_file)
        {
            log_file << line << std::endl;
        }
    }

    std::string trimWhitespace(const std::string &value)
    {
        std::string::size_type first = 0;
        while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        {
            ++first;
        }

        std::string::size_type last = value.size();
        while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        {
            --last;
        }

        return value.substr(first, last - first);
    }

    bool parseIntegerSetting(const std::string &value, int minimum, int &output)
    {
        int parsed_value = 0;
        const char *first = value.data();
        const char *last = first + value.size();
        const std::from_chars_result result = std::from_chars(first, last, parsed_value);
        if (result.ec != std::errc{} || result.ptr != last || parsed_value < minimum)
        {
            return false;
        }

        output = parsed_value;
        return true;
    }

    bool loadConfiguration(AppConfig &config, std::string &failure_reason)
    {
        const std::string path = appFilePath(".env");
        std::ifstream env_file(path);
        if (!env_file)
        {
            failure_reason = "configuration file not found: " + path;
            return false;
        }

        std::string line;
        std::size_t line_number = 0;
        while (std::getline(env_file, line))
        {
            ++line_number;
            if (line_number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            {
                line.erase(0, 3);
            }

            line = trimWhitespace(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            const std::string::size_type separator = line.find('=');
            if (separator == std::string::npos)
            {
                failure_reason = "invalid .env entry on line " + std::to_string(line_number);
                return false;
            }

            const std::string key = trimWhitespace(line.substr(0, separator));
            std::string value = trimWhitespace(line.substr(separator + 1));
            if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
            {
                value = value.substr(1, value.size() - 2);
            }

            if (key == "HUST_USER_ID")
            {
                config.user_id = value;
            }
            else if (key == "HUST_PLAIN_PASSWORD")
            {
                config.plain_password = value;
            }
            else if (key == "HUST_CHECK_INTERVAL_SECONDS")
            {
                int seconds = 0;
                if (!parseIntegerSetting(value, 1, seconds))
                {
                    failure_reason = "HUST_CHECK_INTERVAL_SECONDS must be a positive integer (line " + std::to_string(line_number) + ")";
                    return false;
                }
                config.check_interval = std::chrono::seconds{seconds};
            }
            else if (key == "HUST_FAILURE_THRESHOLD")
            {
                if (!parseIntegerSetting(value, 1, config.failure_threshold))
                {
                    failure_reason = "HUST_FAILURE_THRESHOLD must be an integer of at least 1 (line " + std::to_string(line_number) + ")";
                    return false;
                }
            }
        }

        std::string missing_fields;
        const auto append_missing = [&missing_fields](const std::string &field)
        {
            if (!missing_fields.empty())
            {
                missing_fields += ", ";
            }
            missing_fields += field;
        };

        if (config.user_id.empty())
        {
            append_missing("HUST_USER_ID");
        }
        if (config.plain_password.empty())
        {
            append_missing("HUST_PLAIN_PASSWORD");
        }

        if (!missing_fields.empty())
        {
            failure_reason = "missing .env fields: " + missing_fields;
            return false;
        }

        failure_reason.clear();
        return true;
    }

    bool hasInternetAccess(std::string &failure_reason)
    {
        CURL *curl = curl_easy_init();
        if (curl == nullptr)
        {
            failure_reason = "cannot create a cURL handle";
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, CONNECTIVITY_CHECK_URL);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "HUST-Network-Guard/1.0");

        const CURLcode result = curl_easy_perform(curl);
        long http_code = 0;
        if (result == CURLE_OK)
        {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }
        curl_easy_cleanup(curl);

        if (result != CURLE_OK)
        {
            failure_reason = curl_easy_strerror(result);
            return false;
        }
        if (http_code < 200 || http_code >= 400)
        {
            failure_reason = "connectivity check returned HTTP " + std::to_string(http_code);
            return false;
        }

        failure_reason.clear();
        return true;
    }

    bool checkInternetAccess(std::string &failure_reason)
    {
        const bool report_result = immediate_check_report_pending.exchange(false);
        const bool online = hasInternetAccess(failure_reason);

        if (report_result)
        {
            if (online)
            {
                logMessage("Immediate connectivity check result: online.");
            }
            else
            {
                logMessage("Immediate connectivity check result: offline (" + failure_reason + ").");
            }
        }

        return online;
    }

    bool isHexDigit(char value)
    {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
    }

    std::string formEncode(const std::string &value)
    {
        constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 3);

        for (std::size_t index = 0; index < value.size(); ++index)
        {
            const unsigned char character = static_cast<unsigned char>(value[index]);
            const bool unreserved = std::isalnum(character) || character == '-' ||
                                    character == '_' || character == '.' || character == '~';
            if (unreserved)
            {
                encoded.push_back(static_cast<char>(character));
            }
            else
            {
                encoded.push_back('%');
                encoded.push_back(HEX_DIGITS[(character >> 4) & 0x0F]);
                encoded.push_back(HEX_DIGITS[character & 0x0F]);
            }
        }

        return encoded;
    }

    void appendUtf8(std::string &output, unsigned int code_point)
    {
        if (code_point <= 0x7F)
        {
            output.push_back(static_cast<char>(code_point));
        }
        else if (code_point <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    int hexValue(char value)
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }
        return value - 'A' + 10;
    }

    bool extractJsonStringField(const std::string &json, const std::string &field, std::string &value)
    {
        const std::string key = "\"" + field + "\"";
        std::string::size_type position = json.find(key);
        if (position == std::string::npos)
        {
            return false;
        }

        position += key.size();
        while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])))
        {
            ++position;
        }
        if (position >= json.size() || json[position++] != ':')
        {
            return false;
        }
        while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])))
        {
            ++position;
        }
        if (position >= json.size() || json[position++] != '"')
        {
            return false;
        }

        value.clear();
        while (position < json.size())
        {
            const char character = json[position++];
            if (character == '"')
            {
                return true;
            }
            if (character != '\\')
            {
                value.push_back(character);
                continue;
            }
            if (position >= json.size())
            {
                return false;
            }

            const char escape = json[position++];
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                value.push_back(escape);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
            {
                if (position + 4 > json.size() ||
                    !isHexDigit(json[position]) || !isHexDigit(json[position + 1]) ||
                    !isHexDigit(json[position + 2]) || !isHexDigit(json[position + 3]))
                {
                    return false;
                }
                unsigned int code_point = 0;
                for (int digit = 0; digit < 4; ++digit)
                {
                    code_point = (code_point << 4) | static_cast<unsigned int>(hexValue(json[position + digit]));
                }
                position += 4;
                appendUtf8(value, code_point);
                break;
            }
            default:
                return false;
            }
        }

        return false;
    }

    std::string lowercaseAscii(std::string value)
    {
        for (char &character : value)
        {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        return value;
    }

    std::string logSafeExcerpt(const std::string &value)
    {
        std::string excerpt;
        excerpt.reserve(value.size());
        for (char character : value)
        {
            if (character == '\r' || character == '\n' || character == '\t')
            {
                excerpt.push_back(' ');
            }
            else if (static_cast<unsigned char>(character) >= 0x20)
            {
                excerpt.push_back(character);
            }
            if (excerpt.size() >= 500)
            {
                excerpt += "...";
                break;
            }
        }
        return excerpt;
    }

    std::string percentDecode(const std::string &value)
    {
        std::string decoded;
        decoded.reserve(value.size());
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            if (value[index] == '%' && index + 2 < value.size() &&
                isHexDigit(value[index + 1]) && isHexDigit(value[index + 2]))
            {
                decoded.push_back(static_cast<char>(
                    (hexValue(value[index + 1]) << 4) | hexValue(value[index + 2])));
                index += 2;
            }
            else if (value[index] == '+')
            {
                decoded.push_back(' ');
            }
            else
            {
                decoded.push_back(value[index]);
            }
        }
        return decoded;
    }

    bool extractQueryParameter(const std::string &query, const std::string &name, std::string &value)
    {
        std::string candidate = query;
        for (int decoding_pass = 0; decoding_pass < 3; ++decoding_pass)
        {
            std::string::size_type start = 0;
            while (start <= candidate.size())
            {
                const std::string::size_type end = candidate.find('&', start);
                const std::string part = candidate.substr(
                    start, end == std::string::npos ? std::string::npos : end - start);
                const std::string::size_type separator = part.find('=');
                if (separator != std::string::npos && part.substr(0, separator) == name)
                {
                    value = part.substr(separator + 1);
                    return true;
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }

            const std::string decoded = percentDecode(candidate);
            if (decoded == candidate)
            {
                break;
            }
            candidate = decoded;
        }
        return false;
    }

    std::string extractPortalUrlFromBody(const std::string &body)
    {
        constexpr std::array<const char *, 4> MARKERS{
            "top.self.location.href='",
            "top.self.location.href=\"",
            "location.href='",
            "location.href=\"",
        };

        for (const char *marker : MARKERS)
        {
            const std::string marker_text(marker);
            const std::string::size_type marker_position = body.find(marker_text);
            if (marker_position == std::string::npos)
            {
                continue;
            }

            const std::string::size_type url_start = marker_position + marker_text.size();
            const char quote = marker_text.back();
            const std::string::size_type url_end = body.find(quote, url_start);
            if (url_end != std::string::npos)
            {
                return body.substr(url_start, url_end - url_start);
            }
        }
        return "";
    }

    void replaceHtmlAmpersands(std::string &value)
    {
        constexpr char HTML_AMPERSAND[] = "&amp;";
        std::string::size_type position = 0;
        while ((position = value.find(HTML_AMPERSAND, position)) != std::string::npos)
        {
            value.replace(position, sizeof(HTML_AMPERSAND) - 1, "&");
            ++position;
        }
    }

    bool parsePortalUrl(const std::string &raw_url, PortalParameters &portal, std::string &failure_reason)
    {
        std::string portal_url = raw_url;
        replaceHtmlAmpersands(portal_url);

        CURLU *url = curl_url();
        if (url == nullptr)
        {
            failure_reason = "cannot create a cURL URL parser";
            return false;
        }

        const CURLUcode set_result = curl_url_set(url, CURLUPART_URL, portal_url.c_str(), 0);
        if (set_result != CURLUE_OK)
        {
            failure_reason = std::string("invalid portal URL: ") + curl_url_strerror(set_result);
            curl_url_cleanup(url);
            return false;
        }

        char *scheme = nullptr;
        char *host = nullptr;
        char *port = nullptr;
        char *path = nullptr;
        char *query = nullptr;
        const CURLUcode scheme_result = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
        const CURLUcode host_result = curl_url_get(url, CURLUPART_HOST, &host, 0);
        const CURLUcode port_result = curl_url_get(url, CURLUPART_PORT, &port, 0);
        const CURLUcode path_result = curl_url_get(url, CURLUPART_PATH, &path, 0);
        const CURLUcode query_result = curl_url_get(url, CURLUPART_QUERY, &query, 0);

        const auto cleanup_parts = [&]
        {
            curl_free(scheme);
            curl_free(host);
            curl_free(port);
            curl_free(path);
            curl_free(query);
            curl_url_cleanup(url);
        };

        if (scheme_result != CURLUE_OK || host_result != CURLUE_OK ||
            path_result != CURLUE_OK || query_result != CURLUE_OK)
        {
            failure_reason = "portal URL is missing its scheme, host, path, or query string";
            cleanup_parts();
            return false;
        }

        const std::string scheme_value(scheme);
        const std::string path_value(path);
        if ((scheme_value != "http" && scheme_value != "https") ||
            path_value.find("/eportal/index.jsp") == std::string::npos)
        {
            failure_reason = "portal URL does not point to an eportal login page";
            cleanup_parts();
            return false;
        }

        portal.query_string = query;
        if (!extractQueryParameter(portal.query_string, "mac", portal.mac) ||
            portal.mac.size() != 32)
        {
            failure_reason = "portal query string does not contain a valid MAC token";
            cleanup_parts();
            return false;
        }
        for (char character : portal.mac)
        {
            if (!isHexDigit(character))
            {
                failure_reason = "portal MAC token is not hexadecimal";
                cleanup_parts();
                return false;
            }
        }

        std::string host_value(host);
        if (host_value.find(':') != std::string::npos && host_value.front() != '[')
        {
            host_value = "[" + host_value + "]";
        }
        portal.login_url = scheme_value + "://" + host_value;
        if (port_result == CURLUE_OK && port != nullptr)
        {
            portal.login_url += ":" + std::string(port);
        }
        portal.login_url += "/eportal/InterFace.do?method=login";

        cleanup_parts();
        failure_reason.clear();
        return true;
    }

    bool discoverPortal(PortalParameters &portal, std::string &failure_reason)
    {
        CURL *curl = curl_easy_init();
        if (curl == nullptr)
        {
            failure_reason = "cannot create a cURL handle for portal discovery";
            return false;
        }

        std::string response_body;
        curl_easy_setopt(curl, CURLOPT_URL, PORTAL_DISCOVERY_URL);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "HUST-Network-Guard/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        const CURLcode result = curl_easy_perform(curl);
        long http_code = 0;
        char *redirect_url = nullptr;
        if (result == CURLE_OK)
        {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect_url);
        }

        std::string portal_url;
        if (redirect_url != nullptr)
        {
            portal_url = redirect_url;
        }
        if (portal_url.find("/eportal/index.jsp") == std::string::npos)
        {
            portal_url = extractPortalUrlFromBody(response_body);
        }
        curl_easy_cleanup(curl);

        if (result != CURLE_OK)
        {
            failure_reason = std::string("portal discovery request failed: ") + curl_easy_strerror(result);
            return false;
        }
        if (portal_url.empty() || portal_url.find("/eportal/index.jsp") == std::string::npos)
        {
            failure_reason = "campus portal redirect was not found in the HTTP response (HTTP " + std::to_string(http_code) + ")";
            return false;
        }

        return parsePortalUrl(portal_url, portal, failure_reason);
    }

    bool hexToBytes(const std::string &hex, std::vector<unsigned char> &bytes)
    {
        if (hex.size() % 2 != 0)
        {
            return false;
        }
        bytes.clear();
        bytes.reserve(hex.size() / 2);
        for (std::size_t index = 0; index < hex.size(); index += 2)
        {
            if (!isHexDigit(hex[index]) || !isHexDigit(hex[index + 1]))
            {
                return false;
            }
            bytes.push_back(static_cast<unsigned char>(
                (hexValue(hex[index]) << 4) | hexValue(hex[index + 1])));
        }
        return true;
    }

    bool encryptPassword(const std::string &plain_password, const std::string &mac, std::string &encrypted_password, std::string &failure_reason)
    {
#ifdef _WIN32
        const std::string message = plain_password + ">" + mac;
        std::vector<unsigned char> modulus;
        if (!hexToBytes(PASSWORD_RSA_MODULUS, modulus) || modulus.size() != 128)
        {
            failure_reason = "the built-in RSA modulus is invalid";
            return false;
        }
        if (message.size() > modulus.size())
        {
            failure_reason = "plain password and MAC are too long for the RSA key";
            return false;
        }

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(status))
        {
            failure_reason = "BCryptOpenAlgorithmProvider failed";
            return false;
        }

        constexpr std::array<unsigned char, 3> PUBLIC_EXPONENT{0x01, 0x00, 0x01};
        BCRYPT_RSAKEY_BLOB header{};
        header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        header.BitLength = static_cast<ULONG>(modulus.size() * 8);
        header.cbPublicExp = static_cast<ULONG>(PUBLIC_EXPONENT.size());
        header.cbModulus = static_cast<ULONG>(modulus.size());

        std::vector<unsigned char> key_blob(sizeof(header) + PUBLIC_EXPONENT.size() + modulus.size());
        std::copy_n(reinterpret_cast<const unsigned char *>(&header), sizeof(header), key_blob.begin());
        std::copy(PUBLIC_EXPONENT.begin(), PUBLIC_EXPONENT.end(), key_blob.begin() + sizeof(header));
        std::copy(modulus.begin(), modulus.end(), key_blob.begin() + sizeof(header) + PUBLIC_EXPONENT.size());

        status = BCryptImportKeyPair(
            algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
            key_blob.data(), static_cast<ULONG>(key_blob.size()), 0);
        if (!BCRYPT_SUCCESS(status))
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            failure_reason = "BCryptImportKeyPair failed";
            return false;
        }

        std::vector<unsigned char> input(modulus.size(), 0);
        std::copy(message.begin(), message.end(), input.end() - message.size());
        std::vector<unsigned char> output(modulus.size(), 0);
        ULONG output_size = 0;
        status = BCryptEncrypt(
            key, input.data(), static_cast<ULONG>(input.size()), nullptr,
            nullptr, 0, output.data(), static_cast<ULONG>(output.size()),
            &output_size, BCRYPT_PAD_NONE);

        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!BCRYPT_SUCCESS(status) || output_size != output.size())
        {
            failure_reason = "BCryptEncrypt failed";
            return false;
        }

        constexpr char HEX_DIGITS[] = "0123456789abcdef";
        encrypted_password.clear();
        encrypted_password.reserve(output.size() * 2);
        for (unsigned char byte : output)
        {
            encrypted_password.push_back(HEX_DIGITS[(byte >> 4) & 0x0F]);
            encrypted_password.push_back(HEX_DIGITS[byte & 0x0F]);
        }
        failure_reason.clear();
        return true;
#else
        (void)plain_password;
        (void)mac;
        (void)encrypted_password;
        failure_reason = "password encryption is only supported on Windows";
        return false;
#endif
    }

    LoginAttemptResult sendLoginRequest(const AppConfig &config)
    {
        LoginAttemptResult attempt;
        PortalParameters portal;
        std::string preparation_error;
        if (!discoverPortal(portal, preparation_error))
        {
            std::string connectivity_failure;
            if (hasInternetAccess(connectivity_failure))
            {
                attempt.status = LoginAttemptStatus::already_online;
                attempt.detail = "internet access recovered during portal discovery";
            }
            else
            {
                attempt.status = LoginAttemptStatus::preparation_error;
                attempt.detail = preparation_error;
            }
            return attempt;
        }

        std::string encrypted_password;
        if (!encryptPassword(config.plain_password, portal.mac, encrypted_password, preparation_error))
        {
            attempt.status = LoginAttemptStatus::preparation_error;
            attempt.detail = preparation_error;
            return attempt;
        }

        CURL *curl = curl_easy_init();
        if (curl == nullptr)
        {
            attempt.status = LoginAttemptStatus::network_error;
            attempt.detail = "cannot create a cURL handle";
            return attempt;
        }

        const std::string data =
            "userId=" + formEncode(config.user_id) +
            "&password=" + formEncode(encrypted_password) +
            "&service=" +
            "&queryString=" + formEncode(portal.query_string) +
            "&operatorPwd="
            "&operatorUserId="
            "&validcode="
            "&passwordEncrypt=true";

        curl_slist *headers = nullptr;
        headers = curl_slist_append(
            headers,
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 "
            "Safari/537.36");
        headers = curl_slist_append(headers, "Accept: */*");
        headers = curl_slist_append(
            headers, "Content-Type: application/x-www-form-urlencoded; charset=UTF-8");

        std::string response_body;
        curl_easy_setopt(curl, CURLOPT_URL, portal.login_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        const CURLcode result = curl_easy_perform(curl);
        if (result == CURLE_OK)
        {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &attempt.http_code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (result != CURLE_OK)
        {
            attempt.status = LoginAttemptStatus::network_error;
            attempt.detail = curl_easy_strerror(result);
            return attempt;
        }

        if (attempt.http_code < 200 || attempt.http_code >= 300)
        {
            attempt.status = LoginAttemptStatus::http_error;
            attempt.detail = "HTTP " + std::to_string(attempt.http_code);
            return attempt;
        }

        extractJsonStringField(response_body, "result", attempt.server_result);
        extractJsonStringField(response_body, "message", attempt.server_message);
        if (lowercaseAscii(attempt.server_result) == "success")
        {
            attempt.status = LoginAttemptStatus::accepted;
        }
        else if (!attempt.server_result.empty())
        {
            attempt.status = LoginAttemptStatus::rejected;
        }
        else
        {
            attempt.status = LoginAttemptStatus::unknown_response;
            attempt.detail = logSafeExcerpt(response_body);
        }
        return attempt;
    }

    void logLoginAttempt(const std::string &label, const LoginAttemptResult &attempt)
    {
        std::string message = label + ": ";
        switch (attempt.status)
        {
        case LoginAttemptStatus::accepted:
            message += "accepted by the authentication server";
            break;
        case LoginAttemptStatus::rejected:
            message += "rejected by the authentication server";
            break;
        case LoginAttemptStatus::already_online:
            message += "not run because internet access is already available";
            break;
        case LoginAttemptStatus::network_error:
            message += "network error";
            break;
        case LoginAttemptStatus::http_error:
            message += "HTTP error";
            break;
        case LoginAttemptStatus::preparation_error:
            message += "login preparation error";
            break;
        case LoginAttemptStatus::unknown_response:
            message += "unrecognized authentication response";
            break;
        }

        if (attempt.http_code != 0)
        {
            message += ", HTTP " + std::to_string(attempt.http_code);
        }
        if (!attempt.server_result.empty())
        {
            message += ", result=" + logSafeExcerpt(attempt.server_result);
        }
        if (!attempt.server_message.empty())
        {
            message += ", message=" + logSafeExcerpt(attempt.server_message);
        }
        if (!attempt.detail.empty())
        {
            message += ", detail=" + attempt.detail;
        }
        logMessage(message + ".");
    }

    void processLoginTestIfRequested(const AppConfig &config)
    {
        if (!login_test_requested.exchange(false))
        {
            return;
        }

        std::string connectivity_failure;
        if (hasInternetAccess(connectivity_failure))
        {
            LoginAttemptResult attempt;
            attempt.status = LoginAttemptStatus::already_online;
            attempt.detail = "disconnect this device from the campus network before testing";
            logLoginAttempt("Login configuration test result", attempt);
            notifyLoginTestResult(attempt.status);
            return;
        }

        logMessage("Login configuration test started while offline.");
        const LoginAttemptResult attempt = sendLoginRequest(config);
        logLoginAttempt("Login configuration test result", attempt);
        notifyLoginTestResult(attempt.status);
    }

    struct CurlGlobalCleanup
    {
        ~CurlGlobalCleanup()
        {
            curl_global_cleanup();
        }
    };

    void monitorNetwork()
    {
        AppConfig config;
        std::string configuration_error;
        if (!loadConfiguration(config, configuration_error))
        {
            logMessage("Configuration error: " + configuration_error + ".");
            notifyConfigurationError();
            return;
        }

        const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (init_result != CURLE_OK)
        {
            logMessage(std::string("cURL initialization failed: ") + curl_easy_strerror(init_result));
            return;
        }
        const CurlGlobalCleanup curl_cleanup;

        logMessage("HUST-Network-Guard monitor started.");
        logMessage("Monitor settings: online check interval=" + std::to_string(config.check_interval.count()) + " seconds, failure threshold=" + std::to_string(config.failure_threshold) + ", failure recheck interval=" + std::to_string(FAILURE_RECHECK_INTERVAL.count()) + " seconds.");
        int consecutive_failures = 0;
        bool first_check = true;

        while (monitor_running.load())
        {
            processLoginTestIfRequested(config);

            std::string failure_reason;
            if (checkInternetAccess(failure_reason))
            {
                if (first_check)
                {
                    logMessage("Internet access is available.");
                    notifyConnected();
                }
                else if (consecutive_failures > 0)
                {
                    logMessage("Internet access recovered before login was needed.");
                    notifyConnected();
                }
                first_check = false;
                consecutive_failures = 0;
                if (!waitOrStop(config.check_interval))
                {
                    return;
                }
                continue;
            }

            first_check = false;
            ++consecutive_failures;
            logMessage("Internet check failed (" + std::to_string(consecutive_failures) + "/" + std::to_string(config.failure_threshold) + "): " + failure_reason + ".");

            if (consecutive_failures < config.failure_threshold)
            {
                if (!waitOrStop(FAILURE_RECHECK_INTERVAL))
                {
                    return;
                }
                continue;
            }

            notifyDisconnected();
            std::size_t retry_index = 0;
            while (monitor_running.load())
            {
                logMessage("Offline state confirmed; sending campus login request.");
                const LoginAttemptResult login_attempt = sendLoginRequest(config);
                logLoginAttempt("Automatic login request", login_attempt);

                logMessage("Waiting 5 seconds before verifying connectivity.");
                if (!waitOrStop(LOGIN_VERIFY_DELAY))
                {
                    return;
                }
                processLoginTestIfRequested(config);

                if (checkInternetAccess(failure_reason))
                {
                    logMessage("Campus network login succeeded.");
                    notifyConnected();
                    break;
                }

                const std::chrono::seconds retry_delay = RETRY_DELAYS[retry_index];
                logMessage("Still offline: " + failure_reason + ". Retrying in " + std::to_string(retry_delay.count()) + " seconds.");
                if (!waitOrStop(retry_delay))
                {
                    return;
                }
                processLoginTestIfRequested(config);

                if (checkInternetAccess(failure_reason))
                {
                    logMessage("Internet access recovered while waiting to retry.");
                    notifyConnected();
                    break;
                }

                if (retry_index + 1 < RETRY_DELAYS.size())
                {
                    ++retry_index;
                }
            }

            consecutive_failures = 0;
            if (!waitOrStop(config.check_interval))
            {
                return;
            }
        }
    }

#ifdef _WIN32

    constexpr wchar_t TRAY_WINDOW_CLASS[] = L"HUSTNetworkGuardTrayWindow";
    constexpr UINT TRAY_ICON_ID = 1;
    constexpr UINT TRAY_CALLBACK_MESSAGE = WM_APP + 1;
    constexpr UINT TRAY_CONNECTED_NOTIFICATION_MESSAGE = WM_APP + 2;
    constexpr UINT TRAY_DISCONNECTED_NOTIFICATION_MESSAGE = WM_APP + 3;
    constexpr UINT TRAY_CONFIGURATION_ERROR_MESSAGE = WM_APP + 4;
    constexpr UINT TRAY_LOGIN_TEST_RESULT_MESSAGE = WM_APP + 5;
    constexpr UINT MENU_CHECK_NOW = 1001;
    constexpr UINT MENU_TEST_LOGIN = 1002;
    constexpr UINT MENU_OPEN_LOG = 1003;
    constexpr UINT MENU_EXIT = 1004;

    NOTIFYICONDATAW tray_icon{};
    UINT taskbar_created_message = 0;

    HICON loadApplicationIcon()
    {
        HICON icon = LoadIconA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(IDI_HUST_NETWORK_GUARD));
        if (icon == nullptr)
        {
            icon = LoadIconA(nullptr, IDI_INFORMATION);
        }
        return icon;
    }

    bool addTrayIcon(HWND window)
    {
        tray_icon = {};
        tray_icon.cbSize = sizeof(tray_icon);
        tray_icon.hWnd = window;
        tray_icon.uID = TRAY_ICON_ID;
        tray_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_icon.uCallbackMessage = TRAY_CALLBACK_MESSAGE;
        tray_icon.hIcon = loadApplicationIcon();
        lstrcpynW(tray_icon.szTip, L"HUST-Network-Guard", static_cast<int>(sizeof(tray_icon.szTip) / sizeof(tray_icon.szTip[0])));
        return Shell_NotifyIconW(NIM_ADD, &tray_icon) != FALSE;
    }

    void notifyConnected()
    {
        if (tray_icon.hWnd != nullptr)
        {
            PostMessageW(tray_icon.hWnd, TRAY_CONNECTED_NOTIFICATION_MESSAGE, 0, 0);
        }
    }

    void notifyDisconnected()
    {
        if (tray_icon.hWnd != nullptr)
        {
            PostMessageW(tray_icon.hWnd, TRAY_DISCONNECTED_NOTIFICATION_MESSAGE, 0, 0);
        }
    }

    void notifyConfigurationError()
    {
        if (tray_icon.hWnd != nullptr)
        {
            PostMessageW(tray_icon.hWnd, TRAY_CONFIGURATION_ERROR_MESSAGE, 0, 0);
        }
    }

    void notifyLoginTestResult(LoginAttemptStatus status)
    {
        if (tray_icon.hWnd != nullptr)
        {
            PostMessageW(tray_icon.hWnd, TRAY_LOGIN_TEST_RESULT_MESSAGE, static_cast<WPARAM>(status), 0);
        }
    }

    void showConnectedNotification()
    {
        NOTIFYICONDATAW notification = tray_icon;
        notification.uFlags = NIF_INFO;
        notification.dwInfoFlags = NIIF_INFO;
        notification.uTimeout = 5000;
        lstrcpynW(notification.szInfoTitle, L"HUST-Network-Guard", static_cast<int>(sizeof(notification.szInfoTitle) / sizeof(notification.szInfoTitle[0])));
        lstrcpynW(notification.szInfo, L"校园网已连接，可以正常访问互联网。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    void showDisconnectedNotification()
    {
        NOTIFYICONDATAW notification = tray_icon;
        notification.uFlags = NIF_INFO;
        notification.dwInfoFlags = NIIF_WARNING;
        notification.uTimeout = 5000;
        lstrcpynW(notification.szInfoTitle, L"HUST-Network-Guard", static_cast<int>(sizeof(notification.szInfoTitle) / sizeof(notification.szInfoTitle[0])));
        lstrcpynW(notification.szInfo, L"检测到校园网已断开，正在尝试重新连接。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    void showConfigurationErrorNotification()
    {
        NOTIFYICONDATAW notification = tray_icon;
        notification.uFlags = NIF_INFO;
        notification.dwInfoFlags = NIIF_ERROR;
        notification.uTimeout = 5000;
        lstrcpynW(notification.szInfoTitle, L"HUST-Network-Guard", static_cast<int>(sizeof(notification.szInfoTitle) / sizeof(notification.szInfoTitle[0])));
        lstrcpynW(notification.szInfo, L"无法读取 .env 配置，请检查文件内容后重启程序。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    void showLoginTestNotification(LoginAttemptStatus status)
    {
        NOTIFYICONDATAW notification = tray_icon;
        notification.uFlags = NIF_INFO;
        notification.uTimeout = 7000;
        lstrcpynW(notification.szInfoTitle, L"HUST-Network-Guard", static_cast<int>(sizeof(notification.szInfoTitle) / sizeof(notification.szInfoTitle[0])));

        if (status == LoginAttemptStatus::accepted)
        {
            notification.dwInfoFlags = NIIF_INFO;
            lstrcpynW(notification.szInfo, L"认证服务器已接受当前登录配置。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        }
        else if (status == LoginAttemptStatus::rejected)
        {
            notification.dwInfoFlags = NIIF_WARNING;
            lstrcpynW(notification.szInfo, L"认证服务器拒绝了当前登录配置，请查看日志中的具体原因。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        }
        else if (status == LoginAttemptStatus::already_online)
        {
            notification.dwInfoFlags = NIIF_WARNING;
            lstrcpynW(notification.szInfo, L"当前已经联网，无法验证密码。请先将设备下线再测试。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        }
        else
        {
            notification.dwInfoFlags = NIIF_ERROR;
            lstrcpynW(notification.szInfo, L"登录配置测试未完成，请查看日志中的网络或服务器错误。", static_cast<int>(sizeof(notification.szInfo) / sizeof(notification.szInfo[0])));
        }
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    void openLogFile()
    {
        const std::string path = logFilePath();
        const HINSTANCE result =
            ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            logMessage("Unable to open the log file: " + path + ".");
        }
    }

    void showTrayMenu(HWND window)
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, MENU_CHECK_NOW, L"立即检测");
        AppendMenuW(menu, MF_STRING, MENU_TEST_LOGIN, L"测试登录配置");
        AppendMenuW(menu, MF_STRING, MENU_OPEN_LOG, L"打开日志");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MENU_EXIT, L"退出");

        POINT cursor_position{};
        GetCursorPos(&cursor_position);
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD,
            cursor_position.x, cursor_position.y, 0, window, nullptr);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);

        switch (command)
        {
        case MENU_CHECK_NOW:
            logMessage("An immediate connectivity check was requested.");
            requestImmediateCheck();
            break;
        case MENU_TEST_LOGIN:
            logMessage("A login configuration test was requested.");
            requestLoginConfigurationTest();
            break;
        case MENU_OPEN_LOG:
            openLogFile();
            break;
        case MENU_EXIT:
            DestroyWindow(window);
            break;
        default:
            break;
        }
    }

    LRESULT CALLBACK trayWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (taskbar_created_message != 0 &&
            message == taskbar_created_message)
        {
            addTrayIcon(window);
            return 0;
        }

        switch (message)
        {
        case TRAY_CONNECTED_NOTIFICATION_MESSAGE:
            showConnectedNotification();
            return 0;
        case TRAY_DISCONNECTED_NOTIFICATION_MESSAGE:
            showDisconnectedNotification();
            return 0;
        case TRAY_CONFIGURATION_ERROR_MESSAGE:
            showConfigurationErrorNotification();
            return 0;
        case TRAY_LOGIN_TEST_RESULT_MESSAGE:
            showLoginTestNotification(static_cast<LoginAttemptStatus>(wparam));
            return 0;
        case TRAY_CALLBACK_MESSAGE:
            if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU)
            {
                showTrayMenu(window);
            }
            else if (lparam == WM_LBUTTONDBLCLK)
            {
                logMessage("An immediate connectivity check was requested.");
                requestImmediateCheck();
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &tray_icon);
            stopMonitor();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    HWND createTrayWindow(HINSTANCE instance)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = trayWindowProcedure;
        window_class.hInstance = instance;
        window_class.hIcon = loadApplicationIcon();
        window_class.hIconSm = loadApplicationIcon();
        window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        window_class.lpszClassName = TRAY_WINDOW_CLASS;

        if (RegisterClassExW(&window_class) == 0)
        {
            return nullptr;
        }

        return CreateWindowExW(0, TRAY_WINDOW_CLASS, L"HUST-Network-Guard", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    }

#endif

#ifndef _WIN32

    void notifyConnected() {}
    void notifyDisconnected() {}
    void notifyConfigurationError() {}
    void notifyLoginTestResult(LoginAttemptStatus) {}

#endif

} // namespace

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    HANDLE single_instance_mutex =
        CreateMutexW(nullptr, TRUE, L"Local\\HUSTNetworkGuardSingleInstance");
    if (single_instance_mutex == nullptr)
    {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(single_instance_mutex);
        return 0;
    }

    taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    HWND tray_window = createTrayWindow(instance);
    if (tray_window == nullptr || !addTrayIcon(tray_window))
    {
        MessageBoxW(nullptr, L"无法创建系统托盘图标。",
                    L"HUST-Network-Guard",
                    MB_OK | MB_ICONERROR);
        if (tray_window != nullptr)
        {
            DestroyWindow(tray_window);
        }
        CloseHandle(single_instance_mutex);
        return 1;
    }

    std::thread monitor_thread(monitorNetwork);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    stopMonitor();
    if (monitor_thread.joinable())
    {
        monitor_thread.join();
    }
    CloseHandle(single_instance_mutex);
    return 0;
}

#else

signed main()
{
    monitorNetwork();
    return 0;
}

#endif
