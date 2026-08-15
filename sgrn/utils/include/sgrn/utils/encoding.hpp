#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace sgrn::utils::encoding
{

/**
 * @brief Converts data to a Base64 encoded string.
 */
std::string toBase64(const unsigned char* tp_data, std::size_t t_len);

/**
 * @brief Converts data to a Base64URL encoded string (no padding).
 */
std::string toBase64Url(const unsigned char* tp_data, std::size_t t_len);

std::vector<uint8_t> fromBase64(const std::string& t_input);
/**
 * @brief Converts data to a hexadecimal string.
 */
std::string toHex(const unsigned char* tp_data, std::size_t t_len);

/**
 * @brief Converts a BCD (Binary Coded Decimal) byte to an integer.
 */
int bcdToDec(uint8_t t_bcd);

/**
 * @brief Converts an integer to a BCD (Binary Coded Decimal) byte.
 */
uint8_t decToBcd(int t_dec);

} // namespace sgrn::utils::encoding
