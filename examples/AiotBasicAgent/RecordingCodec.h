#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 DEOTALAND LIMITED (德奧塔文化科技有限公司)
// Adapted from Agent_link-main/firmware/boards/korvo/adpcm_encoder.h and
// firmware/components/agent_link/src/adpcm.h (https://github.com/Deotaland/Agent_link).
// Distributed under the MIT license in this library's LICENSE. The standard
// IMA tables/block layout are retained; C++11 constants replace inline variables,
// decoding validates headers and writes explicitly little-endian PCM bytes.

#include <cstddef>
#include <cstdint>

namespace recording_codec {
static constexpr size_t kBlockBytes = 256;
static constexpr size_t kSamplesPerBlock = 505;
static constexpr size_t kPcmBytesPerBlock = kSamplesPerBlock * 2;
static constexpr int16_t kStepTable[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
    337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static constexpr int8_t kIndexTable[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

inline void update(uint8_t code, int32_t difference, int16_t& predictor, uint8_t& index) {
    int32_t next = int32_t(predictor) + ((code & 8) ? -difference : difference);
    if (next > 32767) next = 32767;
    if (next < -32768) next = -32768;
    predictor = static_cast<int16_t>(next);
    int stepIndex = int(index) + kIndexTable[code];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    index = static_cast<uint8_t>(stepIndex);
}
inline uint8_t encodeSample(int16_t sample, int16_t& predictor, uint8_t& index) {
    const int32_t step = kStepTable[index];
    int32_t difference = int32_t(sample) - predictor;
    uint8_t code = 0;
    if (difference < 0) { code = 8; difference = -difference; }
    int32_t reconstructed = step >> 3;
    if (difference >= step) { code |= 4; difference -= step; reconstructed += step; }
    if (difference >= (step >> 1)) { code |= 2; difference -= step >> 1; reconstructed += step >> 1; }
    if (difference >= (step >> 2)) { code |= 1; reconstructed += step >> 2; }
    update(code, reconstructed, predictor, index);
    return code;
}
inline int16_t decodeSample(uint8_t code, int16_t& predictor, uint8_t& index) {
    const int32_t step = kStepTable[index];
    int32_t difference = step >> 3;
    if (code & 4) difference += step;
    if (code & 2) difference += step >> 1;
    if (code & 1) difference += step >> 2;
    update(code, difference, predictor, index);
    return predictor;
}
inline void putSample(uint8_t* destination, int16_t sample) {
    const uint16_t value = static_cast<uint16_t>(sample);
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}
// Reject a malformed header before indexing the IMA table. The reserved byte is
// zero for standard mono WAV blocks. Samples after the first are low nibble first.
inline bool decodeBlock(const uint8_t* block, size_t bytes, uint8_t* pcm, size_t capacity) {
    if (!block || !pcm || bytes != kBlockBytes || capacity < kPcmBytesPerBlock ||
        block[2] > 88 || block[3] != 0) return false;
    int16_t predictor = static_cast<int16_t>(uint16_t(block[0]) | (uint16_t(block[1]) << 8));
    uint8_t index = block[2];
    putSample(pcm, predictor);
    size_t sample = 1;
    for (size_t i = 4; i < kBlockBytes; ++i) {
        putSample(pcm + 2 * sample++, decodeSample(block[i] & 15, predictor, index));
        putSample(pcm + 2 * sample++, decodeSample(block[i] >> 4, predictor, index));
    }
    return true;
}

class Encoder {
public:
    void reset() { count_ = 0; index_ = 0; }
    size_t pending() const { return count_; }
    bool push(int16_t sample, uint8_t* block) {
        pcm_[count_++] = sample;
        if (count_ != kSamplesPerBlock) return false;
        encodeBlock(block); count_ = 0; return true;
    }
    // Pad by holding the last sample. The WAV fact chunk stores pending() before
    // flush, so padding is never returned by the file reader as recorded audio.
    size_t flush(uint8_t* block) {
        if (!count_) return 0;
        const int16_t hold = pcm_[count_ - 1];
        while (count_ < kSamplesPerBlock) pcm_[count_++] = hold;
        encodeBlock(block); count_ = 0; return kBlockBytes;
    }
private:
    void encodeBlock(uint8_t* output) {
        int16_t predictor = pcm_[0];
        putSample(output, predictor); output[2] = index_; output[3] = 0;
        size_t sample = 1;
        for (size_t i = 4; i < kBlockBytes; ++i) {
            const uint8_t low = encodeSample(pcm_[sample++], predictor, index_);
            const uint8_t high = encodeSample(pcm_[sample++], predictor, index_);
            output[i] = static_cast<uint8_t>(low | (high << 4));
        }
    }
    int16_t pcm_[kSamplesPerBlock] = {};
    size_t count_ = 0;
    uint8_t index_ = 0;
};
}
