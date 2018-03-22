//  Copyright (c) 2018 Surya Priy
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#include <hpx/runtime/components/derived_component_factory.hpp>
#include <hpx/runtime/actions/continuation.hpp>
#include <hpx/runtime/agas/interface.hpp>
#include <hpx/runtime/launch_policy.hpp>
#include <hpx/util/bind.hpp>
#include <hpx/util/format.hpp>
#include <hpx/util/high_resolution_clock.hpp>
#include <hpx/util/unlock_guard.hpp>
#include <hpx/performance_counters/counters.hpp>
#include <hpx/performance_counters/counter_creators.hpp>
#include <hpx/performance_counters/counters_fwd.hpp>
#include <hpx/performance_counters/stubs/performance_counter.hpp>
#include <hpx/performance_counters/server/histogram_performance_counter.hpp>

#include <hpx/util/iterator_range.hpp>

#include <boost/parameter/keyword.hpp>

#if defined(HPX_MSVC)
#  pragma warning(push)
#  pragma warning(disable: 4244)
#endif
#include <boost/accumulators/statistics/median.hpp>
#if defined(HPX_MSVC)
#  pragma warning(pop)
#endif

#include <boost/version.hpp>

#define BOOST_SPIRIT_USE_PHOENIX_V3
#include <boost/spirit/include/qi_char.hpp>
#include <boost/spirit/include/qi_numeric.hpp>
#include <boost/spirit/include/qi_operator.hpp>
#include <boost/spirit/include/qi_parse.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <limits>
#include <utility>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace performance_counters { namespace server
{
    ///////////////////////////////////////////////////////////////////////////
    namespace detail
    {
   
    bool histogram_performance_counter::start()
    {
        return counters_.start(hpx::launch::sync);
    }

    bool histogram_performance_counter::stop()
    {
        return counters_.stop(hpx::launch::sync);
    }

    bool histogram_performance_counter::reset_counter_value()
    {
        counters_.reset(hpx::launch::sync);
    }
        
    }
}}}

namespace hpx { namespace performance_counters { namespace detail
{
    std::vector<std::int64_t>
    histogram_performance_counter::get_histogram(bool reset)
    {
        std::vector<std::int64_t> result;

        std::unique_lock<mutex_type> l(mtx_);
        if (!data_histogram)
        {
            l.unlock();
            HPX_THROW_EXCEPTION(bad_parameter,
                "histogram_performance_counter::"
                    "get_histogram",
                "histogram counter was not initialized for "
                "action type: " + action_name_);
            return result;
        }

        // first add histogram parameters
        result.push_back(histogram_min_boundary_);
        result.push_back(histogram_max_boundary_);
        result.push_back(histogram_num_buckets_);

        auto data = hpx::util::histogram(*data_histogram);
        for (auto const& item : data)
        {
            result.push_back(std::int64_t(item.second * 1000));
        }

        return result;
    }

    void
    histogram_performance_counter::get_histogram_creator(
        std::int64_t min_boundary, std::int64_t max_boundary,
        std::int64_t num_buckets,
        util::function_nonser<std::vector<std::int64_t>(bool)>& result)
    {
        std::lock_guard<mutex_type> l(mtx_);
        if (data_histogram)
        {
            result = util::bind_front(&histogram_performance_counter::
                get_histogram, this);
            return;
        }

        histogram_min_boundary_ = min_boundary;
        histogram_max_boundary_ = max_boundary;
        histogram_num_buckets_ = num_buckets;

        time_between_parcels_.reset(new histogram_collector_type(
            hpx::util::tag::histogram::num_bins = double(num_buckets),
            hpx::util::tag::histogram::min_range = double(min_boundary),
            hpx::util::tag::histogram::max_range = double(max_boundary)));
        last_parcel_time_ = util::high_resolution_clock::now();

        result = util::bind_front(&histogram_performance_counter::
            get_histogram, this);
    }
    
    struct get_histogram_surrogate
    {
        get_histogram_surrogate(
                std::string const& action_name, std::int64_t min_boundary,
            std::int64_t max_boundary, std::int64_t num_buckets)
          : action_name_(action_name), min_boundary_(min_boundary),
            max_boundary_(max_boundary), num_buckets_(num_buckets)
        {}

        get_histogram_surrogate(
                get_histogram_surrogate const& rhs)
          : action_name_(rhs.action_name_), min_boundary_(rhs.min_boundary_),
            max_boundary_(rhs.max_boundary_), num_buckets_(rhs.num_buckets_)
        {}

        std::vector<std::int64_t> operator()(bool reset)
        {
            {
                std::lock_guard<hpx::lcos::local::spinlock> l(mtx_);
                if (counter_.empty())
                {
                    counter_ = registry::instance().
                        get_histogram_counter(action_name_,
                            min_boundary_, max_boundary_, num_buckets_);

                    // no counter available yet
                    if (counter_.empty())
                        return registry::empty_histogram(reset);
                }
            }

            // dispatch to actual counter
            return counter_(reset);
        }

        hpx::lcos::local::spinlock mtx_;
        hpx::util::function_nonser<std::vector<std::int64_t>(bool)> counter_;
        std::string action_name_;
        std::int64_t min_boundary_;
        std::int64_t max_boundary_;
        std::int64_t num_buckets_;
    };

    hpx::naming::gid_type get_histogram_counter_creator(
        hpx::performance_counters::counter_info const& info, hpx::error_code& ec)
    {
        switch (info.type_) {
        case performance_counters::counter_histogram:
            {
                performance_counters::counter_path_elements paths;
                performance_counters::get_counter_path_elements(
                    info.fullname_, paths, ec);
                if (ec) return naming::invalid_gid;

                if (paths.parentinstance_is_basename_) {
                    HPX_THROWS_IF(ec, bad_parameter,
                        "get_histogram_counter_creator",
                        "invalid counter name for "
                        " histogram (instance "
                        "name must not be a valid base counter name)");
                    return naming::invalid_gid;
                }

                if (paths.parameters_.empty())
                {
                    HPX_THROWS_IF(ec, bad_parameter,
                        "get_histogram_counter_creator",
                        "invalid counter parameter for "
                        "histogram: must "
                        "specify an action type");
                    return naming::invalid_gid;
                }

                // split parameters, extract separate values
                std::vector<std::string> params;
                boost::algorithm::split(params, paths.parameters_,
                    boost::algorithm::is_any_of(","),
                    boost::algorithm::token_compress_off);

                std::int64_t min_boundary = 0;
                std::int64_t max_boundary = 1000000;  // 1ms
                std::int64_t num_buckets = 20;

                if (params.empty() || params[0].empty())
                {
                    HPX_THROWS_IF(ec, bad_parameter,
                        "get_histogram_counter_creator",
                        "invalid counter parameter for "
                        "histogram: "
                        "must specify an action type");
                    return naming::invalid_gid;
                }

                if (params.size() > 1 && !params[1].empty())
                    min_boundary = util::safe_lexical_cast<std::int64_t>(params[1]);
                if (params.size() > 2 && !params[2].empty())
                    max_boundary = util::safe_lexical_cast<std::int64_t>(params[2]);
                if (params.size() > 3 && !params[3].empty())
                    num_buckets = util::safe_lexical_cast<std::int64_t>(params[3]);

                // ask registry
                hpx::util::function_nonser<std::vector<std::int64_t>(bool)> f =
                    registry::instance().
                        get_histogram_counter(params[0],
                            min_boundary, max_boundary, num_buckets);

                if (!f.empty())
                {
                    return performance_counters::detail::create_raw_counter(
                        info, std::move(f), ec);
                }

                // the counter is not available yet, create surrogate function
                return performance_counters::detail::create_raw_counter(info,
                    get_histogram_surrogate(
                        params[0], min_boundary, max_boundary, num_buckets), ec);
            }
            break;

        default:
            HPX_THROWS_IF(ec, bad_parameter,
                "get_histogram_counter_creator",
                "invalid counter type requested");
            return naming::invalid_gid;
        }
}}}    
