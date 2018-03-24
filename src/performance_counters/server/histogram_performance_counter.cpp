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
#include <hpx/performance_counters/stubs/performance_counter.hpp>
#include <hpx/performance_counters/server/histogram_counter.hpp>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>

#include <hpx/util/iterator_range.hpp>

#include <boost/parameter/keyword.hpp>
#include <boost/accumulators/framework/accumulator_base.hpp>
#include <boost/accumulators/framework/extractor.hpp>
#include <boost/accumulators/numeric/functional.hpp>
#include <boost/accumulators/framework/parameters/sample.hpp>
#include <boost/accumulators/framework/depends_on.hpp>
#include <boost/accumulators/statistics_fwd.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/min.hpp>

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
   
    }
    
     histogram_performance_counter::histogram_performance_counter(
     counter_info const& info, std::string const& base_counter_name,
     std::int64_t histogram_min_boundary, std::int64_t histogram_max_boundary,
     std::int64_t histogram_num_buckets, bool reset_base_counter)
      : base_type_holder(info),
        timer_(util::bind(&histogram_performance_counter::evaluate, this_()),
            util::bind(&histogram_performance_counter::on_terminate, this_()),
            std::string(action_name) + "_timer", info.fullname_, true),
        base_counter_name_(ensure_counter_prefix(base_counter_name)),
        has_prev_value_(false),
        histogram_min_boundary_(-1),
        histogram_max_boundary_(-1),
        histogram_num_buckets_(-1),
        reset_base_counter_(reset_base_counter)
    {
       registry::instance().register_action(action_name,
            util::bind_front(&histogram_performance_counter::
                get_histogram_creator, this));
            
        if (info.type_ != counter_aggregating) {
            HPX_THROW_EXCEPTION(bad_parameter,
                "histogram_performance_counter::histogram_performance_counter",
                "unexpected counter type specified");
        }

        // make sure this counter starts collecting data
        start();
    }
    
    bool histogram_performance_counter::evaluate()
    {
        // gather current base value
        counter_value base_value;
        if (!evaluate_base_counter(base_value))
            return false;

        // simply average the measured base counter values since it got queried
        // for the last time
        counter_value value;
        if (base_value.scaling_ != prev_value_.scaling_ ||
            base_value.scale_inverse_ != prev_value_.scale_inverse_)
        {
            // not supported right now
            HPX_THROW_EXCEPTION(not_implemented,
                "statistics_counter<Statistic>::evaluate",
                "base counter should keep scaling constant over time");
            return false;
        }
        else {
            // accumulate new value
            std::lock_guard<mutex_type> l(mtx_);
         //   value_->add_value(static_cast<double>(base_value.value_));
            
            // collecting data for histogram
            if(data_histogram)
                (*data_histogram)(static_cast<double>(base_value.value_));
        }
        return true;
    }

    bool histogram_performance_counter::ensure_base_counter()
    {
        // lock here to avoid checking out multiple reference counted GIDs
        // from AGAS. This
        std::unique_lock<mutex_type> l(mtx_);

        if (!base_counter_id_) {
            // get or create the base counter
            error_code ec(lightweight);
            hpx::id_type base_counter_id;
            {
                // We need to unlock the lock here since get_counter might suspend
                util::unlock_guard<std::unique_lock<mutex_type> > unlock(l);
                base_counter_id = get_counter(base_counter_name_, ec);
            }

            // After reacquiring the lock, we need to check again if base_counter_id_
            // hasn't been set yet
            if (!base_counter_id_)
            {
                base_counter_id_ = base_counter_id;
            }
            else
            {
                // If it was set already by a different thread, return true.
                return true;
            }

            if (HPX_UNLIKELY(ec || !base_counter_id_))
            {
                // base counter could not be retrieved
                HPX_THROW_EXCEPTION(bad_parameter,
                    "histogram_performance_counter::evaluate_base_counter",
                    hpx::util::format(
                        "could not get or create performance counter: '%s'",
                        base_counter_name_));
                return false;
            }
        }

        return true;
    }

    bool histogram_performance_counter::evaluate_base_counter(
        counter_value& value)
    {
        // query the actual value
        if (!base_counter_id_ && !ensure_base_counter())
            return false;

        value = stubs::performance_counter::get_value(
            launch::sync, base_counter_id_, reset_base_counter_);

        if (!has_prev_value_)
        {
            has_prev_value_ = true;
            prev_value_ = value;
        }

        return true;
    }

    ///////////////////////////////////////////////////////////////////////////
    // Start and stop this counter. We dispatch the calls to the base counter
    // and control our own interval_timer.
    bool histogram_performance_counter::start()
    {
        if (!timer_.is_started()) {
            // start base counter
            if (!base_counter_id_ && !ensure_base_counter())
                return false;

            bool result = stubs::performance_counter::start(
                launch::sync, base_counter_id_);
            if (result) {
                // acquire the current value of the base counter
                counter_value base_value;
                if (evaluate_base_counter(base_value))
                {
                    std::lock_guard<mutex_type> l(mtx_);
               //   value_->add_value(static_cast<double>(base_value.value_));
                    prev_value_ = base_value;
                    
                   // collecting data for histogram
                   if(data_histogram)
                        (*data_histogram)(static_cast<double>(base_value.value_));                    
                }

                // start timer
                timer_.start();
            }
            else {
                // start timer even if base counter does not support being
                // start/stop operations
                timer_.start(true);
            }
            return result;
        }
        return false;
    }

    bool histogram_performance_counter::stop()
    {
        if (timer_.is_started()) {
            timer_.stop();

            if (!base_counter_id_ && !ensure_base_counter())
                return false;
            return stubs::performance_counter::stop(
                launch::sync, base_counter_id_);
        }
        return false;
    }

    void histogram_performance_counter::reset_counter_value()
    {
        std::lock_guard<mutex_type> l(mtx_);

        // reset accumulator
        // value_.reset(new detail::counter_type_from_histogram(
        //    parameter2_));

        // start off with last base value
       //  value_->add_value(static_cast<double>(prev_value_.value_));

        // collecting data for histogram
        if(data_histogram)
             (*data_histogram)(static_cast<double>(base_value.value_));        
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
