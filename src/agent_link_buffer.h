#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>

namespace agentlink {
template<size_t N> class FixedText {
public:
    FixedText& operator+=(const char* text) {
        if (!text) return *this;
        while (*text && good_) *this += *text++;
        return *this;
    }
    FixedText& operator+=(char c) {
        if (length_==N) good_=false;
        else if (good_) { bytes_[length_++]=c; bytes_[length_]=0; }
        return *this;
    }
    void clear() { length_=0; good_=true; bytes_[0]=0; }
    size_t size() const { return length_; }
    bool empty() const { return length_==0; }
    bool good() const { return good_; }
    const char* data() const { return bytes_; }
private:
    char bytes_[N+1] = {};
    size_t length_=0;
    bool good_=true;
};
// Caller supplies synchronization. Allocate once at stream start, never in push
// or pop; rejection is atomic, so retrying the same chunk cannot duplicate bytes.
class ByteRing {
public:
    ~ByteRing() { delete[] bytes_; }
    bool reserve(size_t capacity) {
        if (bytes_) return capacity == capacity_;
        if (!capacity) return false;
        bytes_=new (std::nothrow) uint8_t[capacity];
        if (!bytes_) return false;
        capacity_=capacity; return true;
    }
    size_t size() const { return count_; }
    bool empty() const { return count_==0; }
    size_t available() const { return capacity_-count_; }
    bool push(const uint8_t* data, size_t n) {
        if (!data || !n || n>available()) return false;
        const size_t tail=(head_+count_)%capacity_;
        const size_t first=n<capacity_-tail ? n : capacity_-tail;
        memcpy(bytes_+tail,data,first);
        if (n>first) memcpy(bytes_,data+first,n-first);
        count_+=n; return true;
    }
    size_t pop(uint8_t* out, size_t capacity) {
        const size_t n=count_<capacity ? count_ : capacity;
        if (!out || !n) return 0;
        const size_t first=n<capacity_-head_ ? n : capacity_-head_;
        memcpy(out,bytes_+head_,first);
        if(n>first) memcpy(out+first,bytes_,n-first);
        head_=(head_+n)%capacity_; count_-=n; return n;
    }
    void clear() { head_=count_=0; }
    void release() { delete[] bytes_; bytes_=nullptr; capacity_=head_=count_=0; }
    ByteRing() = default;
    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;
private:
    uint8_t* bytes_=nullptr;
    size_t capacity_=0, head_=0, count_=0;
};

template<size_t N> class FixedBytes {
public:
    void clear() { length_=0; }
    void assign(const uint8_t* first,const uint8_t* last) {
        length_=static_cast<size_t>(last-first);
        if(length_>N) { length_=0; return; }
        if(length_) memcpy(bytes_,first,length_);
    }
    size_t size() const { return length_; }
    bool empty() const { return length_==0; }
    const uint8_t* data() const { return bytes_; }
private:
    uint8_t bytes_[N] = {};
    size_t length_=0;
};

template<class T,size_t N> class FixedQueue {
public:
    bool empty() const { return count_==0; }
    size_t size() const { return count_; }
    T& front() { return slots_[head_]; }
    bool push(const T& value) {
        if(count_==N) return false;
        slots_[(head_+count_)%N]=value; ++count_; return true;
    }
    void pop() { if(count_) { head_=(head_+1)%N; --count_; } }
private:
    T slots_[N];
    size_t head_=0,count_=0;
};
}
