#pragma once

#include <assert.h>
#include <vector>
#include <list>

using namespace std;

template <typename T>
class StackBuffer {
	static const size_t kInitCount = 32;
public:
	StackBuffer(const size_t _init_count = kInitCount) 
		: max_count_(_init_count)
		, element_size_(sizeof(T))
		, curr_count_(0)
		, buffer_(max_count_ * element_size_) {
		assert(max_count_ != 0);
	}

	template <typename ... TArgs>
	inline T *Get(TArgs ... _args) {
		if (IsFull()) {
			Resize();
		}

		return new (&buffer_[element_size_ * curr_count_++]) T(_args...);
	}

	inline void Pop(void) {
		assert(curr_count_ > 0);
		curr_count_--;
	}

	inline T *Top(void) {
		assert(curr_count_ > 0);
		size_t pos = curr_count_ - 1;
		return reinterpret_cast<T*>(&buffer_[pos * element_size_]);
	}

	inline T *operator[](const size_t _pos) {
		assert(_pos < curr_count_);
		return reinterpret_cast<T*>(&buffer_[_pos * element_size_]);
	}

	inline T *At(const size_t _pos) {
		if (_pos >= curr_count_) {
			return NULL;
		}

		return reinterpret_cast<T*>(&buffer_[_pos * element_size_]);
	}

	inline bool Empty(void) {
		return curr_count_ == 0;
	}

	inline bool IsFull(void) {
		return curr_count_ >= max_count_;
	}

	inline void Clear(void) {
		curr_count_ = 0;
	}

	inline const char *Buffer(void) {
		return &buffer_[0];
	}

	inline const size_t BufferSize(void) {
		return curr_count_ * element_size_;
	}

private:
	inline void Resize(void) {
		max_count_ *= 2;
		buffer_.resize(max_count_ * element_size_);
	}

private:
	size_t max_count_;
	size_t element_size_;
	size_t curr_count_;
	vector<char> buffer_;
};

template <typename T>
class StaticBuffer {
public:
	StaticBuffer(const size_t _max_count) 
		: max_count_(_max_count)
		, element_size_(sizeof(T))
		, curr_count_(0)
		, buffer_(max_count_ * element_size_) {}

	inline void Put(const char *buffer, const size_t size) {
		size_t curr_capacity = curr_count_ * element_size_;
		assert(curr_capacity + size <= buffer_.size());

		bcopy(buffer, &buffer_[curr_capacity], size);
		curr_count_ += size / element_size_;
	}

	inline const T *operator[](const size_t _pos) const {
		assert(_pos < curr_count_);
		return reinterpret_cast<const T*>(&buffer_[_pos * element_size_]);
	}

	inline const T *At(const size_t _pos) const {
		if (_pos >= curr_count_) {
			return NULL;
		}

		return reinterpret_cast<const T*>(&buffer_[_pos * element_size_]);
	}

private:
	size_t max_count_;
	size_t element_size_;
	size_t curr_count_;
	vector<char> buffer_;
};

template <typename T>
class MultiStackBuffer {
	static const size_t kPerAddCount = 1024;
	typedef list<StackBuffer<T> *> StackBufferList;
	typedef vector<StaticBuffer<T> *> StaticBufferVector;
public:
	typedef pair<size_t, T*> ElementPair;

	MultiStackBuffer(const size_t _per_add_count = kPerAddCount) 
		: per_add_count_(_per_add_count)
		, curr_count_(0)
		, curr_stack_(NULL) {
		assert(per_add_count_ != 0);
		Resize();
	}

	~MultiStackBuffer(void) {
		for (typename StackBufferList::const_iterator citr = stack_buffer_list_.begin();
			citr != stack_buffer_list_.end(); ++citr) {
			StackBuffer<T> *stack_buffer = *citr;
			delete stack_buffer;
		}
		stack_buffer_list_.clear();

		for (typename StaticBufferVector::const_iterator citr = records_.begin();
			citr != records_.end(); ++citr) {
			StaticBuffer<T> *static_buffer = *citr;
			delete static_buffer;
		}
		records_.clear();
	}

	template <typename ... TArgs>
	inline ElementPair Get(TArgs ... _args) {
		if (curr_stack_->IsFull()) {
			Resize();
		}

		T *element = curr_stack_->Get(_args...);
		return ElementPair(curr_count_++, element);
	}

	void Save(void) {
		StaticBuffer<T> *static_buffer = new StaticBuffer<T>(curr_count_);

		for (typename StackBufferList::const_iterator citr = stack_buffer_list_.begin();
			citr != stack_buffer_list_.end(); ++citr) {
			StackBuffer<T> *stack_buffer = *citr;
			if (stack_buffer->BufferSize() > 0) {
				static_buffer->Put(stack_buffer->Buffer(), stack_buffer->BufferSize());
			}
		}

		records_.push_back(static_buffer);
	}

	inline size_t GetRecordCount(void) {
		return records_.size();
	}

	inline const StaticBuffer<T> *GetRecordByIndex(size_t _index) {
		if (_index < records_.size()) {
			return records_[_index];
		}

		return NULL;
	}

private:
	inline void Resize(void) {
		curr_stack_ = new StackBuffer<T>(per_add_count_);
		stack_buffer_list_.push_back(curr_stack_);
	}

private:
	size_t per_add_count_;
	size_t curr_count_;

	StackBuffer<T> *curr_stack_;
	StackBufferList stack_buffer_list_;

	StaticBufferVector records_;
};