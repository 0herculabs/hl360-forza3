#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
struct PPCContext;
namespace fm3 {
void InitPlayer2Profile(const std::filesystem::path& root, uint64_t host_xuid);
std::string Player2Name();
void PreparePlayer2Career(PPCContext& ctx, uint8_t* base, uint32_t player);
void SavePlayer2Points(uint32_t event, uint32_t points);
std::string UiLayersSummary(uint8_t* base);
void DescribePlayerGarages(PPCContext& ctx, uint8_t* base);
void PublishRaceView(uint8_t* base);
void ApplyGameSettingBytes(uint8_t* base);
void Player2Result(int32_t out[7]);
}
