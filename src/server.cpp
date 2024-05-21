#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define MSG_SIZE 1024
#define CR "\r\n"
#define CONTENT_TYPE(x) ("Content-Type: " x CR)
#define CONTENT_LENGTH(x) ("Content-Length: " + x + CR)
#define END_HEADER CR
#define OK "HTTP/1.1 200 OK" CR
#define CREATED "HTTP/1.1 201 Created" CR
#define ERR_404 "HTTP/1.1 404 Not Found" CR CR

enum class RequestType {
    UNKNOWN = -1,
    GET,
    POST
};

struct Request {
    struct {
        RequestType type = RequestType::UNKNOWN;
        std::string path;
        std::string ver;
    } main;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    friend std::ostream& operator<<(std::ostream& os, const Request& req);
};

std::ostream& operator<<(std::ostream& os, const Request& req)
{
    switch (req.main.type) {
    case RequestType::GET:
        os << "GET";
        break;
    case RequestType::UNKNOWN:
        break;
    case RequestType::POST:
        os << "POST";
        break;
    }
    os << " " << req.main.path << " " << req.main.ver << std::endl;
    for (auto& e : req.headers) {
        os << "[" << e.first << "]: [" << e.second << "]" << std::endl;
    }
    os << "[Body]: [" << req.body << "]" << std::endl;

    return os;
}

std::optional<std::string> directory = std::nullopt;

std::optional<Request> read_request(char* msg);
std::string generate_response(const Request& request, std::optional<std::string>& payload);
int main(int argc, char** argv)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create server socket\n";
        return 1;
    }

    if (argc > 1) {
        if (strcmp(argv[1], "--directory") == 0) {
            assert(argc > 2);
            directory = argv[2];
        }
    }

    // Since the tester restarts your program quite often, setting REUSE_PORT
    // ensures that we don't run into 'Address already in use' errors
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "setsockopt failed\n";
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(4221);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        std::cerr << "Failed to bind to port 4221\n";
        return 1;
    }

    int connection_backlog = 5;
    if (listen(server_fd, connection_backlog) != 0) {
        std::cerr << "listen failed\n";
        return 1;
    }

    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    while (true) {
        std::cout << "Waiting for a client to connect...\n";

        int client = accept(server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_len);

        std::thread thread([client]() {
            if (client < 0) {
                std::cerr << "Error accepting client\n";
                close(client);
                return;
            }
            std::cout << "Client connected\n";

            char msg[MSG_SIZE];
            int sz = read(client, msg, MSG_SIZE);
            if (sz < 0) {
                std::cerr << "Error reading from client" << std::endl;
                close(client);
                return;
            }

            std::cerr << msg << std::endl;

            auto req = read_request(msg);

            if (req.has_value()) {
                std::cerr << "Generating Response" << std::endl;
                auto response = generate_response(*req, directory);
                std::cerr << response << std::endl;
                write(client, response.c_str(), response.size());
            } else {
                std::cerr << "bad request" << std::endl;
            }
            close(client);
        });
        thread.detach();
    }

    close(server_fd);

    return 0;
}

std::optional<Request> read_request(char* msg)
{
    Request req = {};

    std::vector<std::string_view> request_line;
    // request line
    uint32_t o = 0;
    while (*msg != 0) {
        if (msg[o] == ' ') {
            if (o > 0) {
                request_line.emplace_back(msg, o);
                msg += o + 1; // ignore space
                o = 0;
                continue;
            }
            // ignore spaces
            msg++;
        } else if (msg[o] == '\r' && msg[o + 1] == '\n') {
            request_line.emplace_back(msg, o);
            msg += o + 2; // ignore \n
            break;
        }
        o++;
    }
    if (request_line.size() != 3)
        return std::nullopt;
    if (request_line[0] == "GET") {
        req.main.type = RequestType::GET;
    } else if (request_line[0] == "POST") {
        req.main.type = RequestType::POST;
    } else {
        return std::nullopt;
    }
    req.main.path = request_line[1];
    req.main.ver = request_line[2];

    o = 0;
    std::string key;
    auto& headers = req.headers;
    // headers
    while (*msg != 0) {
        if (key.empty() && msg[o] == ':') {
            key = { msg, o };
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            msg += o + 1; // ignore :
            o = 0;
            continue;
        } else if (msg[o] == '\r' && msg[o + 1] == '\n') {
            if (!key.empty()) {
                headers[key] = { msg, o };
                key = {};
            }
            if (strlen(msg) - o + 3 > 0 && msg[o + 2] == '\r' && msg[o + 3] == '\n') {
                // end of headers
                msg += o + 4;
                break;
            }
            msg += o + 2; // ignore \n
            o = 0;
            continue;
        } else if (o == 0 && msg[o] == ' ') { // trim leading spaces
            msg++;
        }
        o++;
    }
    req.body = msg;

    std::cerr << "Generated request (" << __func__ << ")\n"
              << req << std::endl;

    return req;
}

bool create_file(const std::string& file, const Request& req)
{
    std::cerr << "Creating file: " << file << std::endl;

    if (!directory.has_value()) {
        std::cerr << "Directory not provided" << std::endl;
        return false;
    }

    std::ofstream f;
    f.open(*directory + file);
    if (!f.is_open()) {
        std::cerr << "failed to open file" << std::endl;
        return false;
    }

    if (req.headers.find("content-length") == req.headers.end()) {
        std::cout << "Could not find Content-Length" << std::endl;
        return false;
    }

    size_t num;
    try {
        num = std::stol(req.headers.at("content-length"));
    } catch (std::exception const& e) {
        std::cerr << e.what() << std::endl;
        return false;
    }

    f.write(req.body.c_str(), num);

    return true;
}

std::optional<std::string> find_file(const std::string& file)
{
    std::cerr << "Finding file: " << file << std::endl;

    if (!directory.has_value()) {
        std::cerr << "Directory not provided" << std::endl;
        return std::nullopt;
    }

    std::ifstream f;
    f.open(*directory + file, std::ifstream::ate);
    if (!f.is_open())
        return std::nullopt;
    uint32_t end = f.tellg();
    f.seekg(0);
    std::string s;
    s.resize(end);
    f.read(s.data(), end);
    return s;
}

std::string generate_response(const Request& request, std::optional<std::string>& payload)
{
    std::string response = ERR_404;

    if (request.main.path.empty() || request.main.path[0] != '/') {
        return ERR_404;
    } else if (request.main.path.size() == 1) {
        return OK END_HEADER;
    }

    auto pos = request.main.path.find_last_of('/');
    const std::string& path = request.main.path;
    std::string_view path_with_arg(request.main.path.c_str(), pos);
    std::string_view arg(request.main.path.c_str() + pos + 1);

    switch (request.main.type) {
    case RequestType::GET: {
        if (path_with_arg == "/echo") {
            response = OK;
            response += CONTENT_TYPE("text/plain");
            response += CONTENT_LENGTH(std::to_string(arg.size()));
            response += END_HEADER;
            response += arg;
        } else if (path == "/user-agent") {
            response = OK;
            response += CONTENT_TYPE("text/plain");
            if (request.headers.find("user-agent") == request.headers.end()) {
                std::cerr << "User-Agent not found" << std::endl;
                return ERR_404;
            }
            response += CONTENT_LENGTH(std::to_string(request.headers.at("user-agent").size()));
            response += END_HEADER;
            response += request.headers.at("user-agent");
        } else if (path_with_arg == "/files") {
            auto file = find_file(std::string(arg));

            if (file.has_value()) {
                response = OK;
                response += CONTENT_TYPE("application/octet-stream");
                response += CONTENT_LENGTH(std::to_string(file.value().size()));
                response += END_HEADER;
                response += file.value();
            } else {
                response = ERR_404;
            }
        }
    } break;
    case RequestType::POST: {
        if (path_with_arg == "/files") {
            if (create_file(std::string(arg), request)) {
                response = CREATED;
                response += END_HEADER;
            } else {
                response = ERR_404;
            }
        }
    } break;
    case RequestType::UNKNOWN:
        break;
    }

    return response;
}
