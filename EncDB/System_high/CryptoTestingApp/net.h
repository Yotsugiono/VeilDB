#pragma once
#include <vector>
#include <cstdint>

bool send_buffer(int sockfd, const std::vector<uint8_t>& buf);
bool recv_buffer(int sockfd, std::vector<uint8_t>& buf);
