//  Copyright (c) 2007-2014 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_fwd.hpp>

#include <hpx/util/bind.hpp>

#include <hpx/performance_counters/performance_counter.hpp>
#include <hpx/runtime/actions/continuation.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace performance_counters
{
    ///////////////////////////////////////////////////////////////////////////
    performance_counter::performance_counter(std::string const& name)
      : base_type(performance_counters::get_counter_async(name))
    {}

    performance_counter::performance_counter(std::string const& name,
        hpx::id_type const& locality)
    {
        HPX_ASSERT(naming::is_locality(locality));

        counter_path_elements p;
        get_counter_type_path_elements(name, p);

        std::string full_name;
        p.parentinstanceindex_ = naming::get_locality_id_from_id(locality);
        get_counter_name(p, full_name);

        this->base_type::reset(performance_counters::get_counter_async(full_name));
    }

    ///////////////////////////////////////////////////////////////////////////
    future<counter_info> performance_counter::get_info() const
    {
        return stubs::performance_counter::get_info_async(get_gid());
    }
    counter_info performance_counter::get_info_sync(error_code& ec)
    {
        return stubs::performance_counter::get_info(get_gid(), ec);
    }

    future<counter_value> performance_counter::get_counter_value(bool reset)
    {
        return stubs::performance_counter::get_value_async(get_gid(), reset);
    }
    counter_value performance_counter::get_counter_value_sync(bool reset,
        error_code& ec)
    {
        return stubs::performance_counter::get_value(get_gid(), reset, ec);
    }

    future<counter_value> performance_counter::get_counter_value() const
    {
        return stubs::performance_counter::get_value_async(get_gid(), false);
    }
    counter_value performance_counter::get_counter_value_sync(error_code& ec) const
    {
        return stubs::performance_counter::get_value(get_gid(), false, ec);
    }

    ///////////////////////////////////////////////////////////////////////////
    future<bool> performance_counter::start()
    {
        return stubs::performance_counter::start_async(get_gid());
    }
    bool performance_counter::start_sync(error_code& ec)
    {
        return stubs::performance_counter::start(get_gid(), ec);
    }

    future<bool> performance_counter::stop()
    {
        return stubs::performance_counter::stop_async(get_gid());
    }
    bool performance_counter::stop_sync(error_code& ec)
    {
        return stubs::performance_counter::stop(get_gid(), ec);
    }

    future<void> performance_counter::reset()
    {
        return stubs::performance_counter::reset_async(get_gid());
    }
    void performance_counter::reset_sync(error_code& ec)
    {
        stubs::performance_counter::reset(get_gid(), ec);
    }
}}
