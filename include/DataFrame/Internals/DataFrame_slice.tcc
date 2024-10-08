// Hossein Moein
// September 12, 2017
/*
Copyright (c) 2019-2026, Hossein Moein
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Hossein Moein and/or the DataFrame nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Hossein Moein BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <DataFrame/DataFrame.h>
#include <DataFrame/DataFrameStatsVisitors.h>
#include <DataFrame/Utils/Utils.h>

// ----------------------------------------------------------------------------

namespace hmdf
{

template<typename I, typename H>
template<typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>>
DataFrame<I, H>::get_data_by_idx (Index2D<IndexType> range) const  {

    using res_t = DataFrame<I, HeteroVector<std::size_t(H::align_value)>>;

    const auto  &lower =
        std::lower_bound (indices_.begin(), indices_.end(), range.begin);
    const auto  &upper =
        std::upper_bound (indices_.begin(), indices_.end(), range.end);
    res_t       df;

    if (lower != indices_.end())  {
        df.load_index(lower, upper);

        const size_type b_dist = std::distance(indices_.begin(), lower);
        const size_type e_dist =
            std::distance(indices_.begin(),
                          upper < indices_.end() ? upper : indices_.end());
        const SpinGuard guard(lock_);

        for (const auto &[name, idx] : column_list_) [[likely]]  {
            create_col_functor_<res_t, Ts ...>  functor(name.c_str(), df);

            data_[idx].change(functor);
        }

        const auto  thread_level =
            (indices_.size() < ThreadPool::MUL_THR_THHOLD)
                ? 0L : get_thread_level();
        auto        lbd =
            [b_dist, e_dist, &df, this]
            (const auto &begin, const auto &end) -> void  {
                for (auto citer = begin; citer < end; ++citer)  {
                    load_functor_<res_t, Ts ...>  functor (
                         citer->first.c_str(), b_dist, e_dist, df);

                    this->data_[citer->second].change(functor);
                }
            };

        if (thread_level > 2)  {
            auto    futuers = thr_pool_.parallel_loop(column_list_.begin(),
                                                      column_list_.end(),
                                                      std::move(lbd));

            for (auto &fut : futuers)  fut.get();
        }
        else  {
            lbd(column_list_.begin(), column_list_.end());
        }
    }

    return (df);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_idx(const StlVecType<IndexType> &values) const  {

    static_assert(hashable<I>, "Index type must be hash-able");

    using res_t = DataFrame<I, HeteroVector<std::size_t(H::align_value)>>;

    const DFUnorderedSet<IndexType> val_table (values.begin(), values.end());
    typename res_t::IndexVecType    new_index;
    StlVecType<size_type>           locations;
    const size_type                 values_s = values.size();
    const size_type                 idx_s = indices_.size();

    new_index.reserve(values_s);
    locations.reserve(values_s);
    for (size_type i = 0; i < idx_s; ++i)
        if (val_table.find(indices_[i]) != val_table.end())  {
            new_index.push_back(indices_[i]);
            locations.push_back(i);
        }

    res_t   df;

    df.load_index(std::move(new_index));

    const SpinGuard guard(lock_);

    for (const auto &[name, idx] : column_list_) [[likely]]  {
        create_col_functor_<res_t, Ts ...>  functor(name.c_str(), df);

        data_[idx].change(functor);
    }

    const auto  thread_level =
        (idx_s < ThreadPool::MUL_THR_THHOLD) ? 0L : get_thread_level();

    if (thread_level > 2)  {
        auto    lbd =
            [&locations = std::as_const(locations), idx_s, &df, this]
            (const auto &begin, const auto &end) -> void  {
                for (auto citer = begin; citer < end; ++citer)  {
                    sel_load_functor_<res_t, size_type, Ts ...> functor(
                        citer->first.c_str(),
                        locations,
                        idx_s,
                        df);

                    this->data_[citer->second].change(functor);
                }
            };

        auto    futuers =
            thr_pool_.parallel_loop(column_list_.begin(),
                                    column_list_.end(),
                                    std::move(lbd));

        for (auto &fut : futuers)  fut.get();
    }
    else  {
        for (const auto &[name, idx] : column_list_) [[likely]]  {
            sel_load_functor_<res_t, size_type, Ts ...> functor(
                name.c_str(),
                locations,
                idx_s,
                df);

            data_[idx].change(functor);
        }
    }

    return (df);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::View
DataFrame<I, H>::get_view_by_idx (Index2D<IndexType> range)  {

    const auto  lower =
        std::lower_bound (indices_.begin(), indices_.end(), range.begin);
    const auto  upper =
        std::upper_bound (indices_.begin(), indices_.end(), range.end);
    View        dfv;

    if (lower != indices_.end() &&
        (upper != indices_.end() || indices_.back() == range.end)) [[likely]] {
        IndexType       *upper_address { nullptr };
        const size_type b_dist = std::distance(indices_.begin(), lower);
        const size_type e_dist = std::distance(indices_.begin(), upper);

        if (upper != indices_.end())
            upper_address = &*upper;
        else
            upper_address = &(indices_.front()) + e_dist;
        dfv.indices_ = typename View::IndexVecType(&*lower, upper_address);

        const SpinGuard guard(lock_);

        for (const auto &[name, idx] : column_list_) [[likely]]  {
            view_setup_functor_<View, Ts ...>   functor (name.c_str(),
                                                         b_dist,
                                                         e_dist,
                                                         dfv);

            data_[idx].change(functor);
        }
    }

    return (dfv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstView
DataFrame<I, H>::get_view_by_idx (Index2D<IndexType> range) const  {

    const auto  lower =
        std::lower_bound (indices_.begin(), indices_.end(), range.begin);
    const auto  upper =
        std::upper_bound (indices_.begin(), indices_.end(), range.end);
    ConstView   dfcv;

    if (lower != indices_.end() &&
        (upper != indices_.end() || indices_.back() == range.end)) [[likely]] {
        const IndexType *upper_address = nullptr;
        const size_type b_dist = std::distance(indices_.begin(), lower);
        const size_type e_dist = std::distance(indices_.begin(), upper);

        if (upper != indices_.end())
            upper_address = &*upper;
        else
            upper_address = &(indices_.front()) + e_dist;
        dfcv.indices_ =
            typename ConstView::IndexVecType(&*lower, upper_address);

        const SpinGuard guard(lock_);

        for (const auto &[name, idx] : column_list_) [[likely]]  {
            view_setup_functor_<ConstView, Ts ...>  functor (name.c_str(),
                                                             b_dist,
                                                             e_dist,
                                                             dfcv);

            data_[idx].change(functor);
        }
    }

    return (dfcv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_idx(const StlVecType<IndexType> &values)  {

    using TheView = PtrView;

    const DFUnorderedSet<IndexType> val_table (values.begin(), values.end());
    typename TheView::IndexVecType  new_index;
    StlVecType<size_type>           locations;
    const size_type                 values_s = values.size();
    const size_type                 idx_s = indices_.size();

    new_index.reserve(values_s);
    locations.reserve(values_s);

    for (size_type i = 0; i < idx_s; ++i) [[likely]]
        if (val_table.find(indices_[i]) != val_table.end()) [[likely]]  {
            new_index.push_back(&(indices_[i]));
            locations.push_back(i);
        }

    TheView dfv;

    dfv.indices_ = std::move(new_index);

    const SpinGuard guard(lock_);

    for (const auto &[name, idx] : column_list_) [[likely]]  {
        sel_load_view_functor_<size_type, TheView, Ts ...>   functor (
            name.c_str(),
            locations,
            idx_s,
            dfv);

        data_[idx].change(functor);
    }

    return (dfv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_idx(const StlVecType<IndexType> &values) const  {

    using TheView = ConstPtrView;

    const DFUnorderedSet<IndexType> val_table (values.begin(), values.end());
    typename TheView::IndexVecType  new_index;
    StlVecType<size_type>           locations;
    const size_type                 values_s = values.size();
    const size_type                 idx_s = indices_.size();

    new_index.reserve(values_s);
    locations.reserve(values_s);

    for (size_type i = 0; i < idx_s; ++i) [[likely]]
        if (val_table.find(indices_[i]) != val_table.end()) [[likely]]  {
            new_index.push_back(&(indices_[i]));
            locations.push_back(i);
        }

    TheView dfv;

    dfv.indices_ = std::move(new_index);

    const SpinGuard guard(lock_);

    for (const auto &[name, idx] : column_list_) [[likely]]  {
        sel_load_view_functor_<size_type, TheView, Ts ...>  functor (
            name.c_str(),
            locations,
            idx_s,
            dfv);

        data_[idx].change(functor);
    }

    return (dfv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>>
DataFrame<I, H>::get_data_by_loc (Index2D<long> range) const  {

    using res_t = DataFrame<I, HeteroVector<std::size_t(H::align_value)>>;

    if (range.begin < 0)
        range.begin = static_cast<long>(indices_.size()) + range.begin;
    if (range.end < 0)
        range.end = static_cast<long>(indices_.size()) + range.end + 1;

    if (range.end <= static_cast<long>(indices_.size()) &&
        range.begin <= range.end && range.begin >= 0) [[likely]]  {
        res_t   df;

        df.load_index(indices_.begin() + static_cast<size_type>(range.begin),
                      indices_.begin() + static_cast<size_type>(range.end));

        const SpinGuard guard(lock_);

        for (const auto &[name, idx] : column_list_) [[likely]]  {
            create_col_functor_<res_t, Ts ...>  functor(name.c_str(), df);

            data_[idx].change(functor);
        }

        const auto  thread_level =
            (indices_.size() < ThreadPool::MUL_THR_THHOLD)
                ? 0L : get_thread_level();

        if (thread_level > 2)  {
            auto    lbd =
                [&range = std::as_const(range), &df, this]
                (const auto &begin, const auto &end) -> void  {
                    for (auto citer = begin; citer < end; ++citer)  {
                        load_functor_<res_t, Ts ...>    functor(
                            citer->first.c_str(),
                            static_cast<size_type>(range.begin),
                            static_cast<size_type>(range.end),
                            df);

                        this->data_[citer->second].change(functor);
                    }
                };

            auto    futuers =
                thr_pool_.parallel_loop(column_list_.begin(),
                                        column_list_.end(),
                                        std::move(lbd));

            for (auto &fut : futuers)  fut.get();
        }
        else  {
            for (const auto &[name, idx] : column_list_) [[likely]]  {
                load_functor_<res_t, Ts ...>    functor(
                    name.c_str(),
                    static_cast<size_type>(range.begin),
                    static_cast<size_type>(range.end),
                    df);

                data_[idx].change(functor);
            }
        }

        return (df);
    }

    char buffer [512];

    snprintf (buffer, sizeof(buffer) - 1,
              "DataFrame::get_data_by_loc(): ERROR: "
              "Bad begin, end range: %ld, %ld",
              range.begin, range.end);
    throw BadRange (buffer);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_loc (const StlVecType<long> &locations) const  {

    using res_t = DataFrame<I, HeteroVector<std::size_t(H::align_value)>>;

    const size_type                 idx_s = indices_.size();
    res_t                           df;
    typename res_t::IndexVecType    new_index;

    new_index.reserve(locations.size());
    for (auto citer : locations) [[likely]]  {
        const size_type index =
            citer >= 0 ? (citer) : (citer) + static_cast<long>(idx_s);

        new_index.push_back(indices_[index]);
    }
    df.load_index(std::move(new_index));

    const SpinGuard guard(lock_);

    for (const auto &[name, idx] : column_list_) [[likely]]  {
        create_col_functor_<res_t, Ts ...>  functor (name.c_str(), df);

        data_[idx].change(functor);
    }

    const auto  thread_level =
        (idx_s < ThreadPool::MUL_THR_THHOLD) ? 0L : get_thread_level();

    if (thread_level > 2)  {
        auto    lbd =
            [&locations = std::as_const(locations), idx_s, &df, this]
            (const auto &begin, const auto &end) -> void  {
                for (auto citer = begin; citer < end; ++citer)  {
                    sel_load_functor_<res_t, long, Ts ...>  functor(
                        citer->first.c_str(),
                        locations,
                        idx_s,
                        df);

                    this->data_[citer->second].change(functor);
                }
            };

        auto    futuers =
            thr_pool_.parallel_loop(column_list_.begin(),
                                    column_list_.end(),
                                    std::move(lbd));

        for (auto &fut : futuers)  fut.get();
    }
    else  {
        for (const auto &[name, idx] : column_list_) [[likely]]  {
            sel_load_functor_<res_t, long, Ts ...>  functor(name.c_str(),
                                                            locations,
                                                            idx_s,
                                                            df);

            data_[idx].change(functor);
        }
    }

    return (df);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::View
DataFrame<I, H>::get_view_by_loc (Index2D<long> range)  {

    const long  idx_s = static_cast<long>(indices_.size());

    if (range.begin < 0)
        range.begin = idx_s + range.begin;
    if (range.end < 0)
        range.end = idx_s + range.end + 1;

    if (range.end <= idx_s && range.begin <= range.end &&
        range.begin >= 0) [[likely]]  {
        View    dfv;

        dfv.indices_ =
            typename View::IndexVecType(&(indices_[0]) + range.begin,
                                        &(indices_[0]) + range.end);

        const SpinGuard guard(lock_);

        for (const auto &[name, idx] : column_list_) [[likely]]  {
            view_setup_functor_<View, Ts ...>   functor (
                name.c_str(),
                static_cast<size_type>(range.begin),
                static_cast<size_type>(range.end),
                dfv);

            data_[idx].change(functor);
        }

        return (dfv);
    }

    char buffer [512];

    snprintf (buffer, sizeof(buffer) - 1,
              "DataFrame::get_view_by_loc(): ERROR: "
              "Bad begin, end range: %ld, %ld",
              range.begin, range.end);
    throw BadRange (buffer);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstView
DataFrame<I, H>::get_view_by_loc (Index2D<long> range) const  {

    const long  idx_s = static_cast<long>(indices_.size());

    if (range.begin < 0)
        range.begin = idx_s + range.begin;
    if (range.end < 0)
        range.end = idx_s + range.end + 1;

    if (range.end <= idx_s && range.begin <= range.end &&
        range.begin >= 0) [[likely]]  {
        ConstView   dfcv;

        dfcv.indices_ =
            typename ConstView::IndexVecType(&(indices_[0]) + range.begin,
                                             &(indices_[0]) + range.end);

        const SpinGuard guard(lock_);

        for (const auto &[name, idx] : column_list_) [[likely]]  {
            view_setup_functor_<ConstView, Ts ...>  functor (
                name.c_str(),
                static_cast<size_type>(range.begin),
                static_cast<size_type>(range.end),
                dfcv);

            data_[idx].change(functor);
        }

        return (dfcv);
    }

    char buffer [512];

    snprintf (buffer, sizeof(buffer) - 1,
              "DataFrame::get_view_by_loc(): ERROR: "
              "Bad begin, end range: %ld, %ld",
              range.begin, range.end);
    throw BadRange(buffer);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::PtrView
DataFrame<I, H>::get_view_by_loc (const StlVecType<long> &locations)  {

    using TheView = PtrView;

    TheView         dfv;
    const size_type idx_s = indices_.size();

    typename TheView::IndexVecType  new_index;

    new_index.reserve(locations.size());
    for (auto citer: locations) [[likely]]  {
        const size_type index =
            citer >= 0 ? (citer) : (citer) + static_cast<long>(idx_s);

        new_index.push_back(&(indices_[index]));
    }
    dfv.indices_ = std::move(new_index);

    const SpinGuard guard(lock_);

    for (const auto &[name, idx] : column_list_) [[likely]]  {
        sel_load_view_functor_<long, TheView, Ts ...>   functor (
            name.c_str(),
            locations,
            indices_.size(),
            dfv);

        data_[idx].change(functor);
    }

    return (dfv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_loc (const StlVecType<long> &locations) const  {

    using TheView = ConstPtrView;

    TheView         dfv;
    const size_type idx_s = indices_.size();

    typename TheView::IndexVecType  new_index;

    new_index.reserve(locations.size());
    for (auto citer: locations) [[likely]]  {
        const size_type index =
            citer >= 0 ? (citer) : (citer) + static_cast<long>(idx_s);

        new_index.push_back(&(indices_[index]));
    }
    dfv.indices_ = std::move(new_index);

    const SpinGuard guard(lock_);

    for (const auto &[name, idx] : column_list_) [[likely]]  {
        sel_load_view_functor_<long, TheView, Ts ...>   functor (
            name.c_str(),
            locations,
            indices_.size(),
            dfv);

        data_[idx].change(functor);
    }

    return (dfv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T, typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel (const char *name, F &sel_functor) const  {

    const ColumnVecType<T>  &vec = get_column<T>(name);
    const size_type         idx_s = indices_.size();
    const size_type         col_s = vec.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < col_s; ++i)
        if (sel_functor (indices_[i], vec[i]))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T, typename F, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_sel (const char *name, F &sel_functor)  {

    const ColumnVecType<T>  &vec = get_column<T>(name);
    const size_type         idx_s = indices_.size();
    const size_type         col_s = vec.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < col_s; ++i) [[likely]]
        if (sel_functor (indices_[i], vec[i])) [[unlikely]]
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T, typename F, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_view_by_sel (const char *name, F &sel_functor) const  {

    const ColumnVecType<T>  &vec = get_column<T>(name);
    const size_type         idx_s = indices_.size();
    const size_type         col_s = vec.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < col_s; ++i) [[likely]]
        if (sel_functor (indices_[i], vec[i])) [[unlikely]]
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel (const char *name1, const char *name2, F &sel_functor) const  {

    const size_type         idx_s = indices_.size();
    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         min_col_s = std::min(col_s1, col_s2);
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i], vec1[i], vec2[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor (indices_[i],
                         i < col_s1 ? vec1[i] : get_nan<T1>(),
                         i < col_s2 ? vec2[i] : get_nan<T2>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename F, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_sel (const char *name1, const char *name2, F &sel_functor)  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         min_col_s = std::min(col_s1, col_s2);
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i], vec1[i], vec2[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor (indices_[i],
                         i < col_s1 ? vec1[i] : get_nan<T1>(),
                         i < col_s2 ? vec2[i] : get_nan<T2>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename F, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_sel (const char *name1, const char *name2, F &sel_functor) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         min_col_s = std::min(col_s1, col_s2);
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i], vec1[i], vec2[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor (indices_[i],
                         i < col_s1 ? vec1[i] : get_nan<T1>(),
                         i < col_s2 ? vec2[i] : get_nan<T2>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel (const char *name1,
                 const char *name2,
                 const char *name3,
                 F &sel_functor) const  {

    const size_type         idx_s = indices_.size();
    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         min_col_s = std::min({ col_s1, col_s2, col_s3 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i], vec1[i], vec2[i], vec3[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor (indices_[i],
                         i < col_s1 ? vec1[i] : get_nan<T1>(),
                         i < col_s2 ? vec2[i] : get_nan<T2>(),
                         i < col_s3 ? vec3[i] : get_nan<T3>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename Tuple, typename F, typename... FilterCols>
DataFrame<I, H> DataFrame<I, H>::
get_data_by_sel (F &sel_functor) const  {

    const size_type idx_s = indices_.size();
    // Get columns to std::tuple
    std::tuple      cols_for_filter(
        get_column<typename FilterCols::type>(FilterCols::name) ...);
    // Calculate the max size in all columns
    size_type       col_s = 0;

    std::apply([&](auto && ... col)  {
                   using expander = int[];

                   (void) expander { 0, ((col_s = col.size() > col_s ?
                                         col.size() : col_s), 0) ... };
               },
               cols_for_filter);

    // Get the index of all records that meet the filters
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < col_s; ++i)  {
        std::apply(
            [&](auto && ... col)  {
                if (sel_functor(
            indices_[i],
            (i < col.size()
                ? col[i]
                : get_nan<typename std::decay<decltype(col)>::
                      type::value_type>())...))  { col_indices.push_back(i); }
            },
            cols_for_filter);
    }

    // Get the records based on indices
    DataFrame       df;
    IndexVecType    new_index;

    new_index.reserve(col_indices.size());
    for (const auto &citer: col_indices)
        new_index.push_back(indices_[citer]);
    df.load_index(std::move(new_index));

    const SpinGuard guard(lock_);

    for (const auto &citer : column_list_) [[likely]]  {
        create_col_functor_<DataFrame, Tuple> functor(citer.first.c_str(), df);

        data_[citer.second].change(functor);
    }

    const auto  thread_level =
        (indices_.size() < ThreadPool::MUL_THR_THHOLD)
            ? 0L : get_thread_level();

    if (thread_level > 2)  {
        auto    lbd =
            [&col_indices = std::as_const(col_indices), idx_s, &df, this]
            (const auto &begin, const auto &end) -> void  {
                for (auto citer = begin; citer < end; ++citer)  {
                    sel_load_functor_<DataFrame, size_type, Tuple>  functor(
                        citer->first.c_str(),
                        col_indices,
                        idx_s,
                        df);

                    this->data_[citer->second].change(functor);
                }
            };

        auto    futuers =
            thr_pool_.parallel_loop(column_list_.begin(),
                                    column_list_.end(),
                                    std::move(lbd));

        for (auto &fut : futuers)  fut.get();
    }
    else  {
        for (const auto &citer : column_list_) [[likely]]  {
            sel_load_functor_<DataFrame, size_type, Tuple>  functor(
                citer.first.c_str(),
                col_indices,
                idx_s,
                df);

            data_[citer.second].change(functor);
        }
    }

    return (df);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename Tuple, typename F, typename... FilterCols>
DataFrame<I, H> DataFrame<I, H>::
get_data_by_sel (F &sel_functor, FilterCols && ... filter_cols) const  {

    const size_type idx_s = indices_.size();
    // Get columns to std::tuple
    std::tuple      cols_for_filter(
        get_column<typename std::decay<decltype(filter_cols)>::type::type>(
            filter_cols.col_name())...);
    // Calculate the max size in all columns
    size_type       col_s = 0;

    std::apply([&](auto&&... col) {
                   using expander = int[];
                   (void) expander {0, ((col_s = col.size() > col_s ?
                                         col.size() : col_s), 0) ... };
               },
               cols_for_filter);

    // Get the index of all records that meet the filters
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < col_s; ++i)  {
        std::apply(
            [&](auto&&... col) {
                if (sel_functor(indices_[i],
                                (i < col.size() ?
                                 col[i] :
                                 get_nan<typename std::decay<decltype(col)>
                                 ::type::value_type>())...))
                    col_indices.push_back(i);
            },
            cols_for_filter);
    }

    DataFrame       df;
    IndexVecType    new_index;

    // Get the records based on indices
    new_index.reserve(col_indices.size());
    for (const auto &citer: col_indices)
        new_index.push_back(indices_[citer]);
    df.load_index(std::move(new_index));

    const SpinGuard guard(lock_);

    for (const auto &citer : column_list_) [[likely]]  {
        create_col_functor_<DataFrame, Tuple> functor(citer.first.c_str(), df);

        data_[citer.second].change(functor);
    }

    const auto  thread_level =
        (indices_.size() < ThreadPool::MUL_THR_THHOLD)
            ? 0L : get_thread_level();

    if (thread_level > 2)  {
        auto    lbd =
            [&col_indices = std::as_const(col_indices), idx_s, &df, this]
            (const auto &begin, const auto &end) -> void  {
                for (auto citer = begin; citer < end; ++citer)  {
                    sel_load_functor_<DataFrame, size_type, Tuple>  functor(
                        citer->first.c_str(),
                        col_indices,
                        idx_s,
                        df);

                    this->data_[citer->second].change(functor);
                }
            };

        auto    futuers =
            thr_pool_.parallel_loop(column_list_.begin(),
                                    column_list_.end(),
                                    std::move(lbd));

        for (auto &fut : futuers)  fut.get();
    }
    else  {
        for (const auto &citer : column_list_) [[likely]]  {
            sel_load_functor_<DataFrame, size_type, Tuple>  functor(
                citer.first.c_str(),
                col_indices,
                idx_s,
                df);

            data_[citer.second].change(functor);
        }
    }

    return (df);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename F, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_sel (const char *name1,
                 const char *name2,
                 const char *name3,
                 F &sel_functor)  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         min_col_s = std::min({ col_s1, col_s2, col_s3 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i], vec1[i], vec2[i], vec3[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor (indices_[i],
                         i < col_s1 ? vec1[i] : get_nan<T1>(),
                         i < col_s2 ? vec2[i] : get_nan<T2>(),
                         i < col_s3 ? vec3[i] : get_nan<T3>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename F, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_sel (const char *name1,
                 const char *name2,
                 const char *name3,
                 F &sel_functor) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         min_col_s = std::min({ col_s1, col_s2, col_s3 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i], vec1[i], vec2[i], vec3[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor (indices_[i],
                         i < col_s1 ? vec1[i] : get_nan<T1>(),
                         i < col_s2 ? vec2[i] : get_nan<T2>(),
                         i < col_s3 ? vec3[i] : get_nan<T3>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4,
         typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                F &sel_functor) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4> &vec4 = get_column<T4>(name4, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         col_s4 = vec4.size();
    const size_type         min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4,
         typename F, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                F &sel_functor)  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4> &vec4 = get_column<T4>(name4, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         col_s4 = vec4.size();
    const size_type         min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4,
         typename F, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                F &sel_functor) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4> &vec4 = get_column<T4>(name4, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         col_s4 = vec4.size();
    const size_type         min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4, typename T5,
         typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                const char *name5,
                F &sel_functor) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4> &vec4 = get_column<T4>(name4, false);
    const ColumnVecType<T5> &vec5 = get_column<T5>(name5, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         col_s4 = vec4.size();
    const size_type         col_s5 = vec5.size();
    const size_type         min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4, col_s5 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i],
                        vec4[i], vec5[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>(),
                        i < col_s5 ? vec5[i] : get_nan<T5>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4, typename T5,
         typename T6, typename T7, typename T8, typename T9, typename T10,
         typename T11,
         typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                const char *name5,
                const char *name6,
                const char *name7,
                const char *name8,
                const char *name9,
                const char *name10,
                const char *name11,
                F &sel_functor) const  {

    const SpinGuard          guard (lock_);
    const ColumnVecType<T1>  &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2>  &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3>  &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4>  &vec4 = get_column<T4>(name4, false);
    const ColumnVecType<T5>  &vec5 = get_column<T5>(name5, false);
    const ColumnVecType<T6>  &vec6 = get_column<T6>(name6, false);
    const ColumnVecType<T7>  &vec7 = get_column<T7>(name7, false);
    const ColumnVecType<T8>  &vec8 = get_column<T8>(name8, false);
    const ColumnVecType<T9>  &vec9 = get_column<T9>(name9, false);
    const ColumnVecType<T10> &vec10 = get_column<T10>(name10, false);
    const ColumnVecType<T11> &vec11 = get_column<T11>(name11, false);
    const size_type          idx_s = indices_.size();
    const size_type          col_s1 = vec1.size();
    const size_type          col_s2 = vec2.size();
    const size_type          col_s3 = vec3.size();
    const size_type          col_s4 = vec4.size();
    const size_type          col_s5 = vec5.size();
    const size_type          col_s6 = vec6.size();
    const size_type          col_s7 = vec7.size();
    const size_type          col_s8 = vec8.size();
    const size_type          col_s9 = vec9.size();
    const size_type          col_s10 = vec10.size();
    const size_type          col_s11 = vec11.size();
    const size_type          min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4, col_s5,
                   col_s6, col_s7, col_s8, col_s9, col_s10,
                   col_s11 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i], vec5[i],
                        vec6[i], vec7[i], vec8[i], vec9[i], vec10[i],
                        vec11[i])) [[unlikely]]
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i) [[likely]]
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>(),
                        i < col_s5 ? vec5[i] : get_nan<T5>(),
                        i < col_s6 ? vec6[i] : get_nan<T6>(),
                        i < col_s7 ? vec7[i] : get_nan<T7>(),
                        i < col_s8 ? vec8[i] : get_nan<T8>(),
                        i < col_s9 ? vec9[i] : get_nan<T9>(),
                        i < col_s10 ? vec10[i] : get_nan<T10>(),
                        i < col_s11 ? vec11[i] : get_nan<T11>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4, typename T5,
         typename T6, typename T7, typename T8, typename T9, typename T10,
         typename T11, typename T12,
         typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                const char *name5,
                const char *name6,
                const char *name7,
                const char *name8,
                const char *name9,
                const char *name10,
                const char *name11,
                const char *name12,
                F &sel_functor) const  {

    const SpinGuard          guard (lock_);
    const ColumnVecType<T1>  &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2>  &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3>  &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4>  &vec4 = get_column<T4>(name4, false);
    const ColumnVecType<T5>  &vec5 = get_column<T5>(name5, false);
    const ColumnVecType<T6>  &vec6 = get_column<T6>(name6, false);
    const ColumnVecType<T7>  &vec7 = get_column<T7>(name7, false);
    const ColumnVecType<T8>  &vec8 = get_column<T8>(name8, false);
    const ColumnVecType<T9>  &vec9 = get_column<T9>(name9, false);
    const ColumnVecType<T10> &vec10 = get_column<T10>(name10, false);
    const ColumnVecType<T11> &vec11 = get_column<T11>(name11, false);
    const ColumnVecType<T12> &vec12 = get_column<T12>(name12, false);
    const size_type          idx_s = indices_.size();
    const size_type          col_s1 = vec1.size();
    const size_type          col_s2 = vec2.size();
    const size_type          col_s3 = vec3.size();
    const size_type          col_s4 = vec4.size();
    const size_type          col_s5 = vec5.size();
    const size_type          col_s6 = vec6.size();
    const size_type          col_s7 = vec7.size();
    const size_type          col_s8 = vec8.size();
    const size_type          col_s9 = vec9.size();
    const size_type          col_s10 = vec10.size();
    const size_type          col_s11 = vec11.size();
    const size_type          col_s12 = vec12.size();
    const size_type          min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4, col_s5,
                   col_s6, col_s7, col_s8, col_s9, col_s10,
                   col_s11, col_s12 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i)
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i], vec5[i],
                        vec6[i], vec7[i], vec8[i], vec9[i], vec10[i],
                        vec11[i], vec12[i]))
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i)
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>(),
                        i < col_s5 ? vec5[i] : get_nan<T5>(),
                        i < col_s6 ? vec6[i] : get_nan<T6>(),
                        i < col_s7 ? vec7[i] : get_nan<T7>(),
                        i < col_s8 ? vec8[i] : get_nan<T8>(),
                        i < col_s9 ? vec9[i] : get_nan<T9>(),
                        i < col_s10 ? vec10[i] : get_nan<T10>(),
                        i < col_s11 ? vec11[i] : get_nan<T11>(),
                        i < col_s12 ? vec12[i] : get_nan<T12>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4, typename T5,
         typename T6, typename T7, typename T8, typename T9, typename T10,
         typename T11, typename T12, typename T13,
         typename F, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                const char *name5,
                const char *name6,
                const char *name7,
                const char *name8,
                const char *name9,
                const char *name10,
                const char *name11,
                const char *name12,
                const char *name13,
                F &sel_functor) const  {

    const SpinGuard          guard (lock_);
    const ColumnVecType<T1>  &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2>  &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3>  &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4>  &vec4 = get_column<T4>(name4, false);
    const ColumnVecType<T5>  &vec5 = get_column<T5>(name5, false);
    const ColumnVecType<T6>  &vec6 = get_column<T6>(name6, false);
    const ColumnVecType<T7>  &vec7 = get_column<T7>(name7, false);
    const ColumnVecType<T8>  &vec8 = get_column<T8>(name8, false);
    const ColumnVecType<T9>  &vec9 = get_column<T9>(name9, false);
    const ColumnVecType<T10> &vec10 = get_column<T10>(name10, false);
    const ColumnVecType<T11> &vec11 = get_column<T11>(name11, false);
    const ColumnVecType<T12> &vec12 = get_column<T12>(name12, false);
    const ColumnVecType<T13> &vec13 = get_column<T13>(name13, false);
    const size_type          idx_s = indices_.size();
    const size_type          col_s1 = vec1.size();
    const size_type          col_s2 = vec2.size();
    const size_type          col_s3 = vec3.size();
    const size_type          col_s4 = vec4.size();
    const size_type          col_s5 = vec5.size();
    const size_type          col_s6 = vec6.size();
    const size_type          col_s7 = vec7.size();
    const size_type          col_s8 = vec8.size();
    const size_type          col_s9 = vec9.size();
    const size_type          col_s10 = vec10.size();
    const size_type          col_s11 = vec11.size();
    const size_type          col_s12 = vec12.size();
    const size_type          col_s13 = vec13.size();
    const size_type          min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4, col_s5,
                   col_s6, col_s7, col_s8, col_s9, col_s10,
                   col_s11, col_s12, col_s13 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i)
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i], vec5[i],
                        vec6[i], vec7[i], vec8[i], vec9[i], vec10[i],
                        vec11[i], vec12[i], vec13[i]))
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i)
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>(),
                        i < col_s5 ? vec5[i] : get_nan<T5>(),
                        i < col_s6 ? vec6[i] : get_nan<T6>(),
                        i < col_s7 ? vec7[i] : get_nan<T7>(),
                        i < col_s8 ? vec8[i] : get_nan<T8>(),
                        i < col_s9 ? vec9[i] : get_nan<T9>(),
                        i < col_s10 ? vec10[i] : get_nan<T10>(),
                        i < col_s11 ? vec11[i] : get_nan<T11>(),
                        i < col_s12 ? vec12[i] : get_nan<T12>(),
                        i < col_s13 ? vec13[i] : get_nan<T13>()))
            col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4, typename T5,
         typename F, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                const char *name5,
                F &sel_functor)  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4> &vec4 = get_column<T4>(name4, false);
    const ColumnVecType<T5> &vec5 = get_column<T5>(name5, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         col_s4 = vec4.size();
    const size_type         col_s5 = vec5.size();
    const size_type         min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4, col_s5 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i)
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i], vec5[i]))
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i)
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>(),
                        i < col_s5 ? vec5[i] : get_nan<T5>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename T1, typename T2, typename T3, typename T4, typename T5,
         typename F, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_sel(const char *name1,
                const char *name2,
                const char *name3,
                const char *name4,
                const char *name5,
                F &sel_functor) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T1> &vec1 = get_column<T1>(name1, false);
    const ColumnVecType<T2> &vec2 = get_column<T2>(name2, false);
    const ColumnVecType<T3> &vec3 = get_column<T3>(name3, false);
    const ColumnVecType<T4> &vec4 = get_column<T4>(name4, false);
    const ColumnVecType<T5> &vec5 = get_column<T5>(name5, false);
    const size_type         idx_s = indices_.size();
    const size_type         col_s1 = vec1.size();
    const size_type         col_s2 = vec2.size();
    const size_type         col_s3 = vec3.size();
    const size_type         col_s4 = vec4.size();
    const size_type         col_s5 = vec5.size();
    const size_type         min_col_s =
        std::min({ col_s1, col_s2, col_s3, col_s4, col_s5 });
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i)
        if (sel_functor(indices_[i],
                        vec1[i], vec2[i], vec3[i], vec4[i], vec5[i]))
            col_indices.push_back(i);
    for (size_type i = min_col_s; i < idx_s; ++i)
        if (sel_functor(indices_[i],
                        i < col_s1 ? vec1[i] : get_nan<T1>(),
                        i < col_s2 ? vec2[i] : get_nan<T2>(),
                        i < col_s3 ? vec3[i] : get_nan<T3>(),
                        i < col_s4 ? vec4[i] : get_nan<T4>(),
                        i < col_s5 ? vec5[i] : get_nan<T5>()))
            col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<StringOnly T, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_like(const char *name,
                 const char *pattern,
                 bool case_insensitive,
                 char esc_char) const  {

    const ColumnVecType<T>  &vec = get_column<T>(name);
    const size_type         col_s = vec.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(col_s / 2);
    for (size_type i = 0; i < col_s; ++i)  {
        if constexpr (std::is_same_v<T, std::string> ||
                      std::is_same_v<T, VirtualString>)  {
            if (_like_clause_compare_(pattern,
                                      vec[i].c_str(),
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
        else  {
            if (_like_clause_compare_(pattern,
                                      vec[i],
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
    }

    return (data_by_sel_common_<Ts ...>(col_indices, indices_.size()));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<StringOnly T, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_like(const char *name,
                 const char *pattern,
                 bool case_insensitive,
                 char esc_char)  {

    static_assert(std::is_base_of<HeteroVector<align_value>, H>::value,
                  "Only a StdDataFrame can call get_view_by_like()");

    const ColumnVecType<T>  &vec = get_column<T>(name);
    const size_type         col_s = vec.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(col_s / 2);
    for (size_type i = 0; i < col_s; ++i) [[likely]]  {
        if constexpr (std::is_same_v<T, std::string> ||
                      std::is_same_v<T, VirtualString>)  {
            if (_like_clause_compare_(pattern,
                                      vec[i].c_str(),
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
        else  {
            if (_like_clause_compare_(pattern,
                                      vec[i],
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, indices_.size()));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<StringOnly T, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_view_by_like(const char *name,
                 const char *pattern,
                 bool case_insensitive,
                 char esc_char) const  {

    static_assert(std::is_base_of<HeteroVector<align_value>, H>::value,
                  "Only a StdDataFrame can call get_view_by_like()");

    const ColumnVecType<T>  &vec = get_column<T>(name);
    const size_type         col_s = vec.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(col_s / 2);
    for (size_type i = 0; i < col_s; ++i) [[likely]]  {
        if constexpr (std::is_same_v<T, std::string> ||
                      std::is_same_v<T, VirtualString>)  {
            if (_like_clause_compare_(pattern,
                                      vec[i].c_str(),
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
        else  {
            if (_like_clause_compare_(pattern,
                                      vec[i],
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, indices_.size()));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<StringOnly T, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_like(const char *name1,
                 const char *name2,
                 const char *pattern1,
                 const char *pattern2,
                 bool case_insensitive,
                 char esc_char) const  {

    const size_type         idx_s = indices_.size();
    const SpinGuard         guard (lock_);
    const ColumnVecType<T>  &vec1 = get_column<T>(name1, false);
    const ColumnVecType<T>  &vec2 = get_column<T>(name2, false);
    const size_type         min_col_s = std::min(vec1.size(), vec2.size());
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]  {
        if constexpr (std::is_same_v<T, std::string> ||
                      std::is_same_v<T, VirtualString>)  {
            if (_like_clause_compare_(pattern1,
                                      vec1[i].c_str(),
                                      case_insensitive,
                                      esc_char) &&
                _like_clause_compare_(pattern2,
                                      vec2[i].c_str(),
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
        else  {
            if (_like_clause_compare_(pattern1,
                                      vec1[i],
                                      case_insensitive,
                                      esc_char) &&
                _like_clause_compare_(pattern2,
                                      vec2[i],
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
    }

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<StringOnly T, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_like(const char *name1,
                 const char *name2,
                 const char *pattern1,
                 const char *pattern2,
                 bool case_insensitive,
                 char esc_char)  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T>  &vec1 = get_column<T>(name1, false);
    const ColumnVecType<T>  &vec2 = get_column<T>(name2, false);
    const size_type         idx_s = indices_.size();
    const size_type         min_col_s = std::min(vec1.size(), vec2.size());
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]  {
        if constexpr (std::is_same_v<T, std::string> ||
                      std::is_same_v<T, VirtualString>)  {
            if (_like_clause_compare_(pattern1,
                                      vec1[i].c_str(),
                                      case_insensitive,
                                      esc_char) &&
                _like_clause_compare_(pattern2,
                                      vec2[i].c_str(),
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
        else  {
            if (_like_clause_compare_(pattern1,
                                      vec1[i],
                                      case_insensitive,
                                      esc_char) &&
                _like_clause_compare_(pattern2,
                                      vec2[i],
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<StringOnly T, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_view_by_like(const char *name1,
                 const char *name2,
                 const char *pattern1,
                 const char *pattern2,
                 bool case_insensitive,
                 char esc_char) const  {

    const SpinGuard         guard (lock_);
    const ColumnVecType<T>  &vec1 = get_column<T>(name1, false);
    const ColumnVecType<T>  &vec2 = get_column<T>(name2, false);
    const size_type         idx_s = indices_.size();
    const size_type         min_col_s = std::min(vec1.size(), vec2.size());
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 2);
    for (size_type i = 0; i < min_col_s; ++i) [[likely]]  {
        if constexpr (std::is_same_v<T, std::string> ||
                      std::is_same_v<T, VirtualString>)  {
            if (_like_clause_compare_(pattern1,
                                      vec1[i].c_str(),
                                      case_insensitive,
                                      esc_char) &&
                _like_clause_compare_(pattern2,
                                      vec2[i].c_str(),
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
        else  {
            if (_like_clause_compare_(pattern1,
                                      vec1[i],
                                      case_insensitive,
                                      esc_char) &&
                _like_clause_compare_(pattern2,
                                      vec2[i],
                                      case_insensitive,
                                      esc_char))
                col_indices.push_back(i);
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data_by_rand(random_policy spec, double n, seed_t seed) const  {

    using res_t = DataFrame<I, HeteroVector<std::size_t(H::align_value)>>;

    bool            use_seed = false;
    size_type       n_rows = static_cast<size_type>(n);
    const size_type index_s = indices_.size();

    if (spec == random_policy::num_rows_with_seed)  {
        use_seed = true;
    }
    else if (spec == random_policy::frac_rows_with_seed)  {
        use_seed = true;
        n_rows = static_cast<size_type>(n * double(index_s));
    }
    else if (spec == random_policy::frac_rows_no_seed)  {
        n_rows = static_cast<size_type>(n * double(index_s));
    }

    if (index_s > 0 && n_rows <= index_s) [[likely]]  {
        std::random_device                        rd;
        std::mt19937                              gen(use_seed ? seed : rd());
        std::uniform_int_distribution<size_type>  dis(0, index_s - 1);
        StlVecType<size_type>                     rand_indices(n_rows);

        for (size_type i = 0; i < n_rows; ++i)
            rand_indices[i] = dis(gen);
        std::ranges::sort(rand_indices);

        typename res_t::IndexVecType    new_index;
        size_type                       prev_value { size_type(-1) };

        new_index.reserve(n_rows);
        for (size_type i = 0; i < n_rows; ++i) [[likely]]  {
            if (rand_indices[i] != prev_value) [[likely]]  {
                new_index.push_back(indices_[rand_indices[i]]);
                prev_value = rand_indices[i];
            }
        }

        res_t   df;

        df.load_index(std::move(new_index));

        const SpinGuard guard(lock_);

        for (const auto &citer : column_list_) [[likely]]  {
            create_col_functor_<res_t, Ts ...>  functor(
                citer.first.c_str(), df);

            data_[citer.second].change(functor);
        }

        const auto  thread_level =
            (indices_.size() < ThreadPool::MUL_THR_THHOLD)
                ? 0L : get_thread_level();

        if (thread_level > 2)  {
            auto    lbd =
                [&rand_indices = std::as_const(rand_indices), &df, this]
                (const auto &begin, const auto &end) -> void  {
                    for (auto citer = begin; citer < end; ++citer)  {
                        random_load_data_functor_<res_t, Ts ...>    functor(
                            citer->first.c_str(),
                            rand_indices,
                            df);

                        this->data_[citer->second].change(functor);
                    }
                };

            auto    futuers =
                thr_pool_.parallel_loop(column_list_.begin(),
                                        column_list_.end(),
                                        std::move(lbd));

            for (auto &fut : futuers)  fut.get();
        }
        else  {
            for (const auto &citer : column_list_) [[likely]]  {
                random_load_data_functor_<res_t, Ts ...>    functor(
                    citer.first.c_str(),
                    rand_indices,
                    df);

                data_[citer.second].change(functor);
            }
        }

        return (df);
    }

    char buffer [512];

    snprintf (buffer, sizeof(buffer) - 1,
              "DataFrame::get_data_by_rand(): ERROR: "
#ifdef _MSC_VER
              "Number of rows requested %zu is more than available rows %zu",
#else
              "Number of rows requested %lu is more than available rows %lu",
#endif // _MSC_VER
              n_rows, index_s);
    throw BadRange (buffer);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_by_rand (random_policy spec, double n, seed_t seed)  {

    bool            use_seed = false;
    size_type       n_rows = static_cast<size_type>(n);
    const size_type index_s = indices_.size();

    if (spec == random_policy::num_rows_with_seed)  {
        use_seed = true;
    }
    else if (spec == random_policy::frac_rows_with_seed)  {
        use_seed = true;
        n_rows = static_cast<size_type>(n * double(index_s));
    }
    else if (spec == random_policy::frac_rows_no_seed)  {
        n_rows = static_cast<size_type>(n * double(index_s));
    }

    if (index_s > 0 && n_rows <= index_s) [[likely]]  {
        std::random_device                        rd;
        std::mt19937                              gen(use_seed ? seed : rd());
        std::uniform_int_distribution<size_type>  dis(0, index_s - 1);
        StlVecType<size_type>                     rand_indices(n_rows);

        for (size_type i = 0; i < n_rows; ++i) [[likely]]
            rand_indices[i] = dis(gen);
        std::ranges::sort(rand_indices);

        using TheView = PtrView;

        typename TheView::IndexVecType  new_index;
        size_type                       prev_value { size_type(-1) };

        new_index.reserve(n_rows);
        for (size_type i = 0; i < n_rows; ++i) [[likely]]  {
            if (rand_indices[i] != prev_value) [[likely]]  {
                new_index.push_back(&(indices_[rand_indices[i]]));
                prev_value = rand_indices[i];
            }
        }

        TheView dfv;

        dfv.indices_ = std::move(new_index);

        const SpinGuard guard(lock_);

        for (const auto &iter : column_list_) [[likely]]  {
            random_load_view_functor_<TheView, Ts ...>
                functor (iter.first.c_str(), rand_indices, dfv);

            data_[iter.second].change(functor);
        }

        return (dfv);
    }

    char buffer [512];

    snprintf (buffer, sizeof(buffer) - 1,
              "DataFrame::get_view_by_rand(): ERROR: "
#ifdef _MSC_VER
              "Number of rows requested %zu is more than available rows %zu",
#else
              "Number of rows requested %lu is more than available rows %lu",
#endif // _MSC_VER
              n_rows, index_s);
    throw BadRange (buffer);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstPtrView
DataFrame<I, H>::
get_view_by_rand (random_policy spec, double n, seed_t seed) const  {

    bool            use_seed = false;
    size_type       n_rows = static_cast<size_type>(n);
    const size_type index_s = indices_.size();

    if (spec == random_policy::num_rows_with_seed)  {
        use_seed = true;
    }
    else if (spec == random_policy::frac_rows_with_seed)  {
        use_seed = true;
        n_rows = static_cast<size_type>(n * double(index_s));
    }
    else if (spec == random_policy::frac_rows_no_seed)  {
        n_rows = static_cast<size_type>(n * double(index_s));
    }

    if (index_s > 0 && n_rows <= index_s) [[likely]]  {
        std::random_device                        rd;
        std::mt19937                              gen(use_seed ? seed : rd());
        std::uniform_int_distribution<size_type>  dis(0, index_s - 1);
        StlVecType<size_type>                     rand_indices(n_rows);

        for (size_type i = 0; i < n_rows; ++i) [[likely]]
            rand_indices[i] = dis(gen);
        std::ranges::sort(rand_indices);

        using TheView = ConstPtrView;

        typename TheView::IndexVecType  new_index;
        size_type                       prev_value { size_type(-1) };

        new_index.reserve(n_rows);
        for (size_type i = 0; i < n_rows; ++i) [[likely]]  {
            if (rand_indices[i] != prev_value) [[likely]]  {
                new_index.push_back(&(indices_[rand_indices[i]]));
                prev_value = rand_indices[i];
            }
        }

        TheView dfv;

        dfv.indices_ = std::move(new_index);

        const SpinGuard guard(lock_);

        for (const auto &iter : column_list_) [[likely]]  {
            random_load_view_functor_<TheView, Ts ...>
                functor (iter.first.c_str(), rand_indices, dfv);

            data_[iter.second].change(functor);
        }

        return (dfv);
    }

    char buffer [512];

    snprintf (buffer, sizeof(buffer) - 1,
              "DataFrame::get_view_by_rand(): ERROR: "
#ifdef _MSC_VER
              "Number of rows requested %zu is more than available rows %zu",
#else
              "Number of rows requested %lu is more than available rows %lu",
#endif // _MSC_VER
              n_rows, index_s);
    throw BadRange (buffer);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_data(const StlVecType<const char *> &col_names) const  {

    using res_t = DataFrame<I, HeteroVector<std::size_t(H::align_value)>>;

    res_t   df;

    df.load_index(indices_.begin(), indices_.end());

    const SpinGuard guard(lock_);

    for (const auto &name_citer : col_names) [[likely]]  {
        const auto  citer = column_tb_.find (name_citer);

        if (citer == column_tb_.end())  {
            char buffer [512];

            snprintf(buffer, sizeof(buffer) - 1,
                     "DataFrame::get_data(): ERROR: Cannot find column '%s'",
                     name_citer);
            throw ColNotFound(buffer);
        }

        load_all_functor_<res_t, Ts ...>    functor (name_citer, df);

        data_[citer->second].change(functor);
    }

    return (df);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::View DataFrame<I, H>::
get_view(const StlVecType<const char *> &col_names)  {

    View            dfv;
    const size_type idx_size = indices_.size();

    dfv.indices_ =
        typename View::IndexVecType(&(indices_[0]), &(indices_[0]) + idx_size);

    const SpinGuard guard(lock_);

    for (const auto &name_citer : col_names) [[likely]]  {
        const auto  citer = column_tb_.find (name_citer);

        if (citer == column_tb_.end())  {
            char buffer [512];

            snprintf(buffer, sizeof(buffer) - 1,
                     "DataFrame::get_view(): ERROR: Cannot find column '%s'",
                     name_citer);
            throw ColNotFound(buffer);
        }

        view_setup_functor_<View, Ts ...>   functor(citer->first.c_str(),
                                                    0,
                                                    idx_size,
                                                    dfv);

        data_[citer->second].change(functor);
    }

    return (dfv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstView
DataFrame<I, H>::
get_view(const StlVecType<const char *> &col_names) const  {

    ConstView       dfcv;
    const size_type idx_size = indices_.size();

    dfcv.indices_ =
        typename ConstView::IndexVecType(&(indices_[0]),
                                         &(indices_[0]) + idx_size);

    const SpinGuard guard(lock_);

    for (const auto &name_citer : col_names) [[likely]]  {
        const auto  citer = column_tb_.find (name_citer);

        if (citer == column_tb_.end())  {
            char buffer [512];

            snprintf(buffer, sizeof(buffer) - 1,
                     "DataFrame::get_view(): ERROR: Cannot find column '%s'",
                     name_citer);
            throw ColNotFound(buffer);
        }

        view_setup_functor_<ConstView, Ts ...>  functor (citer->first.c_str(),
                                                         0,
                                                         idx_size,
                                                         dfcv);

        data_[citer->second].change(functor);
    }

    return (dfcv);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_top_n_data(const char *name, size_type n) const  {

    using res_t = DataFrame<I, HeteroVector<align_value>>;
    using visitor_t = NLargestVisitor<T, I>;

    res_t   result;

    top_n_common_<T, visitor_t, res_t, Ts ...>(name, visitor_t { n }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_top_n_view(const char *name, size_type n)  {

    using res_t = PtrView;
    using visitor_t = NLargestVisitor<T, I>;

    res_t   result;

    top_n_common_<T, visitor_t, res_t, Ts ...>(name, visitor_t { n }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_top_n_view(const char *name, size_type n) const  {

    using res_t = ConstPtrView;
    using visitor_t = NLargestVisitor<T, I>;

    res_t   result;

    top_n_common_<T, visitor_t, res_t, Ts ...>(name, visitor_t { n }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_bottom_n_data(const char *name, size_type n) const  {

    using res_t = DataFrame<I, HeteroVector<align_value>>;
    using visitor_t = NSmallestVisitor<T, I>;

    res_t   result;

    top_n_common_<T, visitor_t, res_t, Ts ...>(name, visitor_t { n }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_bottom_n_view(const char *name, size_type n)  {

    using res_t = PtrView;
    using visitor_t = NSmallestVisitor<T, I>;

    res_t   result;

    top_n_common_<T, visitor_t, res_t, Ts ...>(name, visitor_t { n }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_bottom_n_view(const char *name, size_type n) const  {

    using res_t = ConstPtrView;
    using visitor_t = NSmallestVisitor<T, I>;

    res_t   result;

    top_n_common_<T, visitor_t, res_t, Ts ...>(name, visitor_t { n }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_above_quantile_data(const char *col_name, double quantile) const  {

    using res_t = DataFrame<I, HeteroVector<align_value>>;
    using comp_func_t = std::greater_equal<T>;

    res_t   result;

    above_quantile_common_<T, comp_func_t, res_t, Ts ...>
        (col_name, quantile, comp_func_t { }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_above_quantile_view(const char *col_name, double quantile)  {

    using res_t = PtrView;
    using comp_func_t = std::greater_equal<T>;

    res_t   result;

    above_quantile_common_<T, comp_func_t, res_t, Ts ...>
        (col_name, quantile, comp_func_t { }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_above_quantile_view(const char *col_name, double quantile) const  {

    using res_t = ConstPtrView;
    using comp_func_t = std::greater_equal<T>;

    res_t   result;

    above_quantile_common_<T, comp_func_t, res_t, Ts ...>
        (col_name, quantile, comp_func_t { }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
DataFrame<I, HeteroVector<std::size_t(H::align_value)>> DataFrame<I, H>::
get_below_quantile_data(const char *col_name, double quantile) const  {

    using res_t = DataFrame<I, HeteroVector<align_value>>;
    using comp_func_t = std::less<T>;

    res_t   result;

    above_quantile_common_<T, comp_func_t, res_t, Ts ...>
        (col_name, quantile, comp_func_t { }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_below_quantile_view(const char *col_name, double quantile)  {

    using res_t = PtrView;
    using comp_func_t = std::less<T>;

    res_t   result;

    above_quantile_common_<T, comp_func_t, res_t, Ts ...>
        (col_name, quantile, comp_func_t { }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<comparable T, typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_below_quantile_view(const char *col_name, double quantile) const  {

    using res_t = ConstPtrView;
    using comp_func_t = std::less<T>;

    res_t   result;

    above_quantile_common_<T, comp_func_t, res_t, Ts ...>
        (col_name, quantile, comp_func_t { }, result);
    return (result);
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<DateTime, HeteroVector<std::size_t(H::align_value)>>
DataFrame<I, H>::
get_data_at_times(DateTime::HourType hr,
                  DateTime::MinuteType mn,
                  DateTime::SecondType sc,
                  DateTime::MillisecondType msc) const  {

    static_assert(std::is_base_of<DateTime, I>::value,
                  "Index type must be DateTime to call get_data_at_time()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)
        if (indices_[i].hour() == hr &&
            indices_[i].minute() == mn &&
            indices_[i].sec() == sc &&
            indices_[i].msec() == msc)  col_indices.push_back(i);

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_at_times(DateTime::HourType hr,
                  DateTime::MinuteType mn,
                  DateTime::SecondType sc,
                  DateTime::MillisecondType msc)  {

    static_assert(std::is_base_of<DateTime, I>::value,
                  "Index type must be DateTime to call get_view_at_times()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)
        if (indices_[i].hour() == hr &&
            indices_[i].minute() == mn &&
            indices_[i].sec() == sc &&
            indices_[i].msec() == msc)  col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_view_at_times(DateTime::HourType hr,
                  DateTime::MinuteType mn,
                  DateTime::SecondType sc,
                  DateTime::MillisecondType msc) const  {

    static_assert(std::is_base_of<DateTime, I>::value,
                  "Index type must be DateTime to call get_view_at_times()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)
        if (indices_[i].hour() == hr &&
            indices_[i].minute() == mn &&
            indices_[i].sec() == sc &&
            indices_[i].msec() == msc)  col_indices.push_back(i);

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<DateTime, HeteroVector<std::size_t(H::align_value)>>
DataFrame<I, H>::
get_data_before_times(DateTime::HourType hr,
                      DateTime::MinuteType mn,
                      DateTime::SecondType sc,
                      DateTime::MillisecondType msc) const  {

    static_assert(
        std::is_base_of<DateTime, I>::value,
        "Index type must be DateTime to call get_data_before_time()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)  {
        if (indices_[i].hour() < hr)  {
            col_indices.push_back(i);
        }
        else if (indices_[i].hour() == hr)  {
            if (indices_[i].minute() < mn)  {
                col_indices.push_back(i);
            }
            else if (indices_[i].minute() == mn)  {
                if (indices_[i].sec() < sc)  {
                    col_indices.push_back(i);
                }
                else if (indices_[i].sec() == sc)  {
                    if (indices_[i].msec() < msc)  {
                        col_indices.push_back(i);
                    }
                }
            }
        }
    }

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_before_times(DateTime::HourType hr,
                      DateTime::MinuteType mn,
                      DateTime::SecondType sc,
                      DateTime::MillisecondType msc)  {

    static_assert(
        std::is_base_of<DateTime, I>::value,
        "Index type must be DateTime to call get_view_before_times()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)  {
        if (indices_[i].hour() < hr)  {
            col_indices.push_back(i);
        }
        else if (indices_[i].hour() == hr)  {
            if (indices_[i].minute() < mn)  {
                col_indices.push_back(i);
            }
            else if (indices_[i].minute() == mn)  {
                if (indices_[i].sec() < sc)  {
                    col_indices.push_back(i);
                }
                else if (indices_[i].sec() == sc)  {
                    if (indices_[i].msec() < msc)  {
                        col_indices.push_back(i);
                    }
                }
            }
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_view_before_times(DateTime::HourType hr,
                      DateTime::MinuteType mn,
                      DateTime::SecondType sc,
                      DateTime::MillisecondType msc) const  {

    static_assert(
        std::is_base_of<DateTime, I>::value,
        "Index type must be DateTime to call get_view_before_times()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)  {
        if (indices_[i].hour() < hr)  {
            col_indices.push_back(i);
        }
        else if (indices_[i].hour() == hr)  {
            if (indices_[i].minute() < mn)  {
                col_indices.push_back(i);
            }
            else if (indices_[i].minute() == mn)  {
                if (indices_[i].sec() < sc)  {
                    col_indices.push_back(i);
                }
                else if (indices_[i].sec() == sc)  {
                    if (indices_[i].msec() < msc)  {
                        col_indices.push_back(i);
                    }
                }
            }
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
DataFrame<DateTime, HeteroVector<std::size_t(H::align_value)>>
DataFrame<I, H>::
get_data_after_times(DateTime::HourType hr,
                     DateTime::MinuteType mn,
                     DateTime::SecondType sc,
                     DateTime::MillisecondType msc) const  {

    static_assert(
        std::is_base_of<DateTime, I>::value,
        "Index type must be DateTime to call get_data_after_time()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)  {
        if (indices_[i].hour() > hr)  {
            col_indices.push_back(i);
        }
        else if (indices_[i].hour() == hr)  {
            if (indices_[i].minute() > mn)  {
                col_indices.push_back(i);
            }
            else if (indices_[i].minute() == mn)  {
                if (indices_[i].sec() > sc)  {
                    col_indices.push_back(i);
                }
                else if (indices_[i].sec() == sc)  {
                    if (indices_[i].msec() > msc)  {
                        col_indices.push_back(i);
                    }
                }
            }
        }
    }

    return (data_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::PtrView DataFrame<I, H>::
get_view_after_times(DateTime::HourType hr,
                     DateTime::MinuteType mn,
                     DateTime::SecondType sc,
                     DateTime::MillisecondType msc)  {

    static_assert(
        std::is_base_of<DateTime, I>::value,
        "Index type must be DateTime to call get_view_after_times()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)  {
        if (indices_[i].hour() > hr)  {
            col_indices.push_back(i);
        }
        else if (indices_[i].hour() == hr)  {
            if (indices_[i].minute() > mn)  {
                col_indices.push_back(i);
            }
            else if (indices_[i].minute() == mn)  {
                if (indices_[i].sec() > sc)  {
                    col_indices.push_back(i);
                }
                else if (indices_[i].sec() == sc)  {
                    if (indices_[i].msec() > msc)  {
                        col_indices.push_back(i);
                    }
                }
            }
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

// ----------------------------------------------------------------------------

template<typename I, typename H>
template<typename ... Ts>
typename DataFrame<I, H>::ConstPtrView DataFrame<I, H>::
get_view_after_times(DateTime::HourType hr,
                     DateTime::MinuteType mn,
                     DateTime::SecondType sc,
                     DateTime::MillisecondType msc) const  {

    static_assert(
        std::is_base_of<DateTime, I>::value,
        "Index type must be DateTime to call get_view_after_times()");

    const size_type         idx_s = indices_.size();
    StlVecType<size_type>   col_indices;

    col_indices.reserve(idx_s / 5);
    for (size_type i = 0; i < idx_s; ++i)  {
        if (indices_[i].hour() > hr)  {
            col_indices.push_back(i);
        }
        else if (indices_[i].hour() == hr)  {
            if (indices_[i].minute() > mn)  {
                col_indices.push_back(i);
            }
            else if (indices_[i].minute() == mn)  {
                if (indices_[i].sec() > sc)  {
                    col_indices.push_back(i);
                }
                else if (indices_[i].sec() == sc)  {
                    if (indices_[i].msec() > msc)  {
                        col_indices.push_back(i);
                    }
                }
            }
        }
    }

    return (view_by_sel_common_<Ts ...>(col_indices, idx_s));
}

} // namespace hmdf

// ----------------------------------------------------------------------------

// Local Variables:
// mode:C++
// tab-width:4
// c-basic-offset:4
// End:
