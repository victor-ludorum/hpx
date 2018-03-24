//  Copyright (c) 2018 Surya Priy
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_PERFORMANCE_COUNTERS_SERVER_HISTOGRAM_PERFORMANCE_COUNTER)
#define HPX_PERFORMANCE_COUNTERS_SERVER_HISTOGRAM_PERFORMANCE_COUNTER

#include <hpx/config.hpp>
#include <hpx/lcos/local/spinlock.hpp>
#include <hpx/performance_counters/server/base_performance_counter.hpp>
#include <hpx/runtime/components/server/component_base.hpp>
#include <hpx/runtime/naming/id_type.hpp>
#include <hpx/util/interval_timer.hpp>
#include <hpx/util/detail/pp/nargs.hpp>
#include <hpx/util/function.hpp>
#include <hpx/util/histogram.hpp>
#include <hpx/util/pool_timer.hpp>

#include <boost/smart_ptr/scoped_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <hpx/config/warnings_prefix.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace performance_counters { namespace server
{
     namespace detail
    {
        struct counter_type_from_histogram_base
        {
            virtual ~counter_type_from_histogram_base() {}

            virtual bool need_reset() const = 0;
            virtual double get_value() = 0;
            virtual void add_value(double value) = 0;
        };
    }
    
    ///////////////////////////////////////////////////////////////////////////
    // This counter exposes the histogram for counters processed during the
    // given base time interval. The counter relies on querying a steadily
    // growing counter value.
    class histogram_performance_counter
    {
       typedef hpx::lcos::local::spinlock mutex_type;
        
        // avoid warnings about using this in member initializer list
        histogram_performance_counter* this_() { return this; }

    public:
        histogram_counter() {}
        typedef histogram_performance_counter type_holder;
        typedef base_performance_counter base_type_holder;

        std::vector<std::int64_t>
            get_histogram(bool reset);
        void get_histogram_creator(
            std::int64_t min_boundary, std::int64_t max_boundary,
            std::int64_t num_buckets,
            util::function_nonser<std::vector<std::int64_t>(bool)>& result);
        
        // register the given action
        static void register_action(char const* action, error_code& ec);
        
        get_histogram_values_type get_histogram_counter(
            std::string const& name, std::int64_t min_boundary,
            std::int64_t max_boundary, naming::gid_type& gid,
            std::int64_t num_buckets);
        
            histogram_performance_counter(counter_info const& info,
            std::string const& base_counter_name,
            std::int64_t min_boundary, std::int64_t max_boundary,
            std::int64_t num_buckets, bool reset_base_counter);

        bool start();

        bool stop();

        void reset_counter_value();

        void on_terminate() {}

        /// \brief finalize() will be called just before the instance gets
        ///        destructed
        void finalize()
        {
            base_performance_counter::finalize();
            base_type::finalize();
        }
        
    protected:
        bool evaluate_base_counter(counter_value& value);
        bool evaluate();
        bool ensure_base_counter();

    private:
        typedef lcos::local::spinlock mutex_type;
        mutable mutex_type mtx_;

        hpx::util::interval_timer timer_; ///< base time interval in milliseconds
        std::string base_counter_name_;   ///< name of base counter to be queried
        naming::id_type base_counter_id_;

        boost::scoped_ptr<detail::counter_type_from_histogram_base> value_;
        counter_value prev_value_;
        bool has_prev_value_;
        
        bool reset_base_counter_;
        
        typedef boost::accumulators::accumulator_set<
                double,     // collects percentiles
                boost::accumulators::features<hpx::util::tag::histogram>
            > histogram_collector_type;

        std::unique_ptr<histogram_collector_type> data_histogram;
        std::int64_t histogram_min_boundary_;
        std::int64_t histogram_max_boundary_;
        std::int64_t histogram_num_buckets_;
        
        // base counters to be queried
        performance_counter_set counters_;
        
    };
}}}

#endif
