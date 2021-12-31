// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/rowset/segment_v2/binary_prefix_page.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "storage/rowset/segment_v2/binary_prefix_page.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>

#include "column/column.h"
#include "common/logging.h"
#include "gutil/strings/substitute.h"
#include "runtime/mem_pool.h"
#include "util/coding.h"
#include "util/faststring.h"
#include "util/slice.h"

namespace starrocks::segment_v2 {

using strings::Substitute;

size_t BinaryPrefixPageBuilder::add(const uint8_t* vals, size_t add_count) {
    DCHECK(!_finished);
    if (add_count == 0) {
        return 0;
    }

    const Slice* src = reinterpret_cast<const Slice*>(vals);
    if (_count == 0) {
        _first_entry.assign_copy(reinterpret_cast<const uint8_t*>(src->get_data()), src->get_size());
    }

    int i = 0;
    for (; i < add_count; ++i, ++src) {
        if (is_page_full()) {
            break;
        }
        const char* entry = src->data;
        size_t entry_len = src->size;
        int old_size = _buffer.size();

        int share_len;
        if (_count % RESTART_POINT_INTERVAL == 0) {
            share_len = 0;
            _restart_points_offset.push_back(old_size);
        } else {
            int max_share_len = std::min(_last_entry.size(), entry_len);
            share_len = max_share_len;
            for (int j = 0; j < max_share_len; ++j) {
                if (entry[j] != _last_entry[j]) {
                    share_len = j;
                    break;
                }
            }
        }
        int non_share_len = entry_len - share_len;

        put_varint32(&_buffer, share_len);
        put_varint32(&_buffer, non_share_len);
        _buffer.append(entry + share_len, non_share_len);

        _last_entry.clear();
        _last_entry.append(entry, entry_len);

        ++_count;
    }
    return i;
}

faststring* BinaryPrefixPageBuilder::finish() {
    DCHECK(!_finished);
    _finished = true;
    put_fixed32_le(&_buffer, (uint32_t)_count);
    uint8_t restart_point_internal = RESTART_POINT_INTERVAL;
    _buffer.append(&restart_point_internal, 1);
    auto restart_point_size = _restart_points_offset.size();
    for (uint32_t i = 0; i < restart_point_size; ++i) {
        put_fixed32_le(&_buffer, _restart_points_offset[i]);
    }
    put_fixed32_le(&_buffer, restart_point_size);
    return &_buffer;
}

template <FieldType Type>
const uint8_t* BinaryPrefixPageDecoder<Type>::_decode_value_lengths(const uint8_t* ptr, uint32_t* shared,
                                                                    uint32_t* non_shared) {
    if ((ptr = decode_varint32_ptr(ptr, _footer_start, shared)) == nullptr) {
        return nullptr;
    }
    if ((ptr = decode_varint32_ptr(ptr, _footer_start, non_shared)) == nullptr) {
        return nullptr;
    }
    if (_footer_start - ptr < *non_shared) {
        return nullptr;
    }
    return ptr;
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::_read_next_value() {
    if (_cur_pos >= _num_values) {
        return Status::NotFound("no more value to read");
    }
    uint32_t shared_len;
    uint32_t non_shared_len;
    auto data_ptr = _decode_value_lengths(_next_ptr, &shared_len, &non_shared_len);
    if (data_ptr == nullptr) {
        return Status::Corruption(strings::Substitute("Failed to decode value at position $0", _cur_pos));
    }
    _current_value.resize(shared_len);
    _current_value.append(data_ptr, non_shared_len);
    _next_ptr = data_ptr + non_shared_len;
    return Status::OK();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::_seek_to_restart_point(size_t restart_point_index) {
    _cur_pos = restart_point_index * _restart_point_internal;
    _next_ptr = _get_restart_point(restart_point_index);
    return _read_next_value();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::init() {
    _cur_pos = 0;
    _next_ptr = reinterpret_cast<const uint8_t*>(_data.get_data());

    const uint8_t* end = _next_ptr + _data.get_size();
    _num_restarts = decode_fixed32_le(end - 4);
    _restarts_ptr = end - (_num_restarts + 1) * 4;
    _footer_start = _restarts_ptr - 4 - 1;
    _num_values = decode_fixed32_le(_footer_start);
    _restart_point_internal = decode_fixed8(_footer_start + 4);
    _parsed = true;
    return _read_next_value();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::seek_to_position_in_page(size_t pos) {
    DCHECK(_parsed);
    DCHECK_LE(pos, _num_values);

    // seek past the last value is valid
    if (pos == _num_values) {
        _cur_pos = _num_values;
        return Status::OK();
    }

    size_t restart_point_index = pos / _restart_point_internal;
    RETURN_IF_ERROR(_seek_to_restart_point(restart_point_index));
    while (_cur_pos < pos) {
        _cur_pos++;
        RETURN_IF_ERROR(_read_next_value());
    }
    return Status::OK();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::seek_at_or_after_value(const void* value, bool* exact_match) {
    DCHECK(_parsed);
    Slice target = *reinterpret_cast<const Slice*>(value);

    uint32_t left = 0;
    uint32_t right = _num_restarts;
    // find the first restart point >= target. after loop,
    // - left == index of first restart point >= target when found
    // - left == _num_restarts when not found (all restart points < target)
    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        // read first entry at restart point `mid`
        RETURN_IF_ERROR(_seek_to_restart_point(mid));
        Slice mid_entry(_current_value);
        if (mid_entry.compare(target) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    // then linear search from the last restart pointer < target.
    // when left == 0, all restart points >= target, so search from first one.
    // otherwise search from the last restart point < target, which is left - 1
    uint32_t search_index = left > 0 ? left - 1 : 0;
    RETURN_IF_ERROR(_seek_to_restart_point(search_index));
    while (true) {
        int cmp = Slice(_current_value).compare(target);
        if (cmp >= 0) {
            *exact_match = cmp == 0;
            return Status::OK();
        }
        _cur_pos++;
        RETURN_IF_ERROR(_read_next_value());
    }
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::_read_next_value_to_output(Slice prev, MemPool* mem_pool, Slice* output) {
    if (_cur_pos >= _num_values) {
        return Status::NotFound("no more value to read");
    }
    uint32_t shared_len;
    uint32_t non_shared_len;
    auto data_ptr = _decode_value_lengths(_next_ptr, &shared_len, &non_shared_len);
    if (data_ptr == nullptr) {
        return Status::Corruption(strings::Substitute("Failed to decode value at position $0", _cur_pos));
    }

    output->size = shared_len + non_shared_len;
    if (output->size > 0) {
        output->data = (char*)mem_pool->allocate(output->size);
        if (UNLIKELY(output->data == nullptr)) {
            return Status::InternalError("Mem usage has exceed the limit of BE");
        }
        memcpy(output->data, prev.data, shared_len);
        memcpy(output->data + shared_len, data_ptr, non_shared_len);
    }

    _next_ptr = data_ptr + non_shared_len;
    return Status::OK();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::_copy_current_to_output(MemPool* mem_pool, Slice* output) {
    output->size = _current_value.size();
    if (output->size > 0) {
        output->data = (char*)mem_pool->allocate(output->size);
        if (output->data == nullptr) {
            return Status::MemoryAllocFailed(strings::Substitute("failed to allocate $0 bytes", output->size));
        }
        memcpy(output->data, _current_value.data(), output->size);
    }
    return Status::OK();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::next_batch(size_t* n, ColumnBlockView* dst) {
    DCHECK(_parsed);
    if (PREDICT_FALSE(*n == 0 || _cur_pos >= _num_values)) {
        *n = 0;
        return Status::OK();
    }
    size_t i = 0;
    size_t max_fetch = std::min(*n, static_cast<size_t>(_num_values - _cur_pos));
    auto out = reinterpret_cast<Slice*>(dst->data());
    auto prev = out;

    // first copy the current value to output
    RETURN_IF_ERROR(_copy_current_to_output(dst->pool(), out));
    i++;
    out++;

    // read and copy remaining values
    for (; i < max_fetch; ++i) {
        _cur_pos++;
        RETURN_IF_ERROR(_read_next_value_to_output(prev[i - 1], dst->pool(), out));
        out++;
    }

    //must update _current_value
    _current_value.clear();
    _current_value.assign_copy((uint8_t*)prev[i - 1].data, prev[i - 1].size);

    *n = max_fetch;
    return Status::OK();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::_next_value(faststring* value) {
    if (_cur_pos >= _num_values) {
        return Status::NotFound("no more value to read");
    }
    uint32_t shared_len;
    uint32_t non_shared_len;
    auto data_ptr = _decode_value_lengths(_next_ptr, &shared_len, &non_shared_len);
    if (data_ptr == nullptr) {
        return Status::Corruption(Substitute("Failed to decode value at position $0", _cur_pos));
    }
    value->resize(shared_len);
    value->append(data_ptr, non_shared_len);
    _next_ptr = data_ptr + non_shared_len;
    ++_cur_pos;
    return Status::OK();
}

template <FieldType Type>
Status BinaryPrefixPageDecoder<Type>::next_batch(size_t* n, vectorized::Column* dst) {
    DCHECK(_parsed);
    if (PREDICT_FALSE(_cur_pos >= _num_values)) {
        *n = 0;
        return Status::OK();
    }
    *n = std::min(*n, static_cast<size_t>(_num_values - _cur_pos));
    // FIXME: ???
    [[maybe_unused]] bool ok = dst->append_strings({_current_value});
    DCHECK(ok);
    if constexpr (Type == OLAP_FIELD_TYPE_CHAR) {
        for (size_t i = 1; i < *n; ++i) {
            RETURN_IF_ERROR(_next_value(&_current_value));
            // remove trailing zeros.
            size_t len = strnlen(reinterpret_cast<const char*>(_current_value.data()), _current_value.size());
            (void)dst->append_strings({Slice(_current_value.data(), len)});
        }
    } else {
        for (size_t i = 1; i < *n; ++i) {
            RETURN_IF_ERROR(_next_value(&_current_value));
            (void)dst->append_strings({_current_value});
        }
    }
    return Status::OK();
}

template class BinaryPrefixPageDecoder<OLAP_FIELD_TYPE_CHAR>;
template class BinaryPrefixPageDecoder<OLAP_FIELD_TYPE_VARCHAR>;

} // namespace starrocks::segment_v2