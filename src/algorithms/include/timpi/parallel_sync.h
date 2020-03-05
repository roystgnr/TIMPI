// The TIMPI Message-Passing Parallelism Library.
// Copyright (C) 2002-2019 Benjamin S. Kirk, John W. Peterson, Roy H. Stogner

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA



#ifndef TIMPI_PARALLEL_SYNC_H
#define TIMPI_PARALLEL_SYNC_H

// Local Includes
#include "timpi/parallel_implementation.h"

// C++ includes
#include <map>
#include <type_traits>
#include <vector>
#include <list>


namespace TIMPI {

//------------------------------------------------------------------------
/**
 * Send and receive and act on vectors of data.
 *
 * The \p data map is indexed by processor ids as keys, and for each
 * processor id in the map there should be a vector of data to send.
 *
 * Data which is received from other processors will be operated on by
 * act_on_data(processor_id_type pid, const std::vector<datum> & data)
 *
 * No guarantee about operation ordering is made - this function will
 * attempt to act on data in the order in which it is received.
 *
 * All receives and actions are completed before this function
 * returns.
 *
 * Note: it is very important that the message tag be completely
 * unique to each invocation
 */
template <typename MapToVectors,
          typename ActionFunctor>
void push_parallel_vector_data(const Communicator & comm,
                               const MapToVectors & data,
                               const ActionFunctor & act_on_data);

/**
 * Send query vectors, receive and answer them with vectors of data,
 * then act on those answers.
 *
 * The \p data map is indexed by processor ids as keys, and for each
 * processor id in the map there should be a vector of query ids to send.
 *
 * Query data which is received from other processors will be operated
 * on by
 * gather_data(processor_id_type pid, const std::vector<id> & ids,
 *             std::vector<datum> & data)
 *
 * Answer data which is received from other processors will be operated on by
 * act_on_data(processor_id_type pid, const std::vector<id> & ids,
 *             const std::vector<datum> & data);
 *
 * The example pointer may be null; it merely needs to be of the
 * correct type.  It's just here because function overloading in C++
 * is easy, whereas SFINAE is hard and partial template specialization
 * of functions is impossible.
 *
 * No guarantee about operation ordering is made - this function will
 * attempt to act on data in the order in which it is received.
 *
 * All receives and actions are completed before this function
 * returns.
 */
template <typename datum,
          typename MapToVectors,
          typename GatherFunctor,
          typename ActionFunctor>
void pull_parallel_vector_data(const Communicator & comm,
                               const MapToVectors & queries,
                               GatherFunctor & gather_data,
                               ActionFunctor & act_on_data,
                               const datum * example);

/**
* Send and receive and act on vectors of data. Similar to
* push_parallel_vector_data, except the vectors are packed and unpacked
* using the Parallel::Packing routines.
*
* The \p data map is indexed by processor ids as keys, and for each
* processor id in the map there should be a vector of data to send.
*
* Data which is received from other processors will be operated on by
* act_on_data(processor_id_type pid, const std::vector<datum> & data)
*
* No guarantee about operation ordering is made - this function will
* attempt to act on data in the order in which it is received.
*
* All receives and actions are completed before this function
* returns.
*
* Note: it is very important that the message tag be completely
* unique to each invocation
*/
template <typename MapToVectors,
          typename ActionFunctor,
          typename ReceiveContext>
void push_parallel_packed_range(const Communicator & comm,
                                const MapToVectors & data,
                                ReceiveContext * receive_context,
                                const ActionFunctor & act_on_data);

//------------------------------------------------------------------------
// Parallel function overloads
//

/*
 * A specialization for types that are harder to non-blocking receive.
 */
template <typename datum,
          typename A,
          typename MapToVectors,
          typename GatherFunctor,
          typename ActionFunctor>
void pull_parallel_vector_data(const Communicator & comm,
                               const MapToVectors & queries,
                               GatherFunctor & gather_data,
                               ActionFunctor & act_on_data,
                               const std::vector<datum,A> * example);






//------------------------------------------------------------------------
// Parallel members
//

template <typename MapToVectors,
          typename ActionFunctor>
void push_parallel_vector_data(const Communicator & comm,
                               const MapToVectors & data,
                               const ActionFunctor & act_on_data)
{
  // This function must be run on all processors at once
  timpi_parallel_only(comm);

  // This function implements the "NBX" algorithm from
  // https://htor.inf.ethz.ch/publications/img/hoefler-dsde-protocols.pdf

  typedef decltype(data.begin()->second.front()) ref_type;
  typedef typename std::remove_reference<ref_type>::type nonref_type;
  typedef typename std::remove_const<nonref_type>::type nonconst_nonref_type;

  // We'll grab a tag so we can overlap request sends and receives
  // without confusing one for the other
  auto tag = comm.get_unique_tag();

  MapToVectors received_data;

  // Post all of the sends, non-blocking and synchronous

  // Save off the old send_mode so we can restore it after this
  auto old_send_mode = comm.send_mode();

  // Set the sending to synchronous - this is so that we can know when
  // the sends are complete
  const_cast<Communicator &>(comm).send_mode(Communicator::SYNCHRONOUS);

  // The send requests
  std::list<Request> reqs;

  processor_id_type num_procs = comm.size();

  for (auto & datapair : data)
    {
      // In the case of data partitioned into more processors than we
      // have ranks, we "wrap around"
      processor_id_type destid = datapair.first % num_procs;
      auto & datum = datapair.second;

      // Just act on data if the user requested a send-to-self
      if (destid == comm.rank())
        act_on_data(destid, datum);
      else
        {
          Request sendreq;
          comm.send(destid, datum, sendreq, tag);
          reqs.push_back(sendreq);
        }
    }

  // In serial we've now acted on all our data.
  if (comm.size() == 1)
    return;

  bool sends_complete = reqs.empty();
  bool started_barrier = false;
  Request barrier_request;

  // Receive

  // The pair of src_pid and requests
  std::list<std::pair<unsigned int, std::shared_ptr<Request>>> receive_reqs;
  auto current_request = std::make_shared<Request>();

  std::multimap<processor_id_type, std::shared_ptr<std::vector<nonconst_nonref_type>>> incoming_data;
  auto current_incoming_data = std::make_shared<std::vector<nonconst_nonref_type>>();

  unsigned int current_src_proc = 0;

  // Keep looking for receives
  while (true)
  {
    // Look for data from anywhere
    current_src_proc = any_source;

    // Check if there is a message and start receiving it
    if (comm.possibly_receive(current_src_proc, *current_incoming_data, *current_request, tag))
    {
      receive_reqs.emplace_back(current_src_proc, current_request);
      current_request = std::make_shared<Request>();

      // current_src_proc will now hold the src pid for this receive
      incoming_data.emplace(current_src_proc, current_incoming_data);
      current_incoming_data = std::make_shared<std::vector<nonconst_nonref_type>>();
    }

    // Clean up outstanding receive requests
    receive_reqs.remove_if([&act_on_data, &incoming_data](std::pair<unsigned int, std::shared_ptr<Request>> & pid_req_pair)
                           {
                             auto & pid = pid_req_pair.first;
                             auto & req = pid_req_pair.second;

                             // If it's finished - let's act on it
                             if (req->test())
                             {
                               // Do any post-wait work
                               req->wait();

                               auto it = incoming_data.find(pid);
                               timpi_assert(it != incoming_data.end());

                               act_on_data(pid, *it->second);

                               // Don't need this data anymore
                               incoming_data.erase(it);

                               // This removes it from the list
                               return true;
                             }

                             // Not finished yet
                             return false;
                           });

    reqs.remove_if([](Request & req)
                   {
                     if (req.test())
                     {
                       // Do Post-Wait work
                       req.wait();

                       return true;
                     }

                     // Not finished yet
                     return false;
                   });


    // See if all of the sends are finished
    if (reqs.empty())
      sends_complete = true;

    // If they've all completed then we can start the barrier
    if (sends_complete && !started_barrier)
    {
      started_barrier = true;
      comm.nonblocking_barrier(barrier_request);
    }

    // Must fully receive everything before being allowed to move on!
    if (receive_reqs.empty())
      // See if all proessors have finished all sends (i.e. _done_!)
      if (started_barrier)
        if (barrier_request.test())
          break; // Done!
  }

  // Reset the send mode
  const_cast<Communicator &>(comm).send_mode(old_send_mode);
}


template <typename MapToVectors,
          typename ActionFunctor,
          typename ReceiveContext>
void push_parallel_packed_range(const Communicator & comm,
                                const MapToVectors & data,
                                ReceiveContext * receive_context,
                                const ActionFunctor & act_on_data)
{
  // This function must be run on all processors at once
  timpi_parallel_only(comm);

  // This function implements the "NBX" algorithm from
  // https://htor.inf.ethz.ch/publications/img/hoefler-dsde-protocols.pdf

  typedef decltype(data.begin()->second.front()) ref_type;
  typedef typename std::remove_reference<ref_type>::type nonref_type;
  typedef typename std::remove_const<nonref_type>::type nonconst_nonref_type;

  // We'll grab a tag so we can overlap request sends and receives
  // without confusing one for the other
  auto tag = comm.get_unique_tag();

  MapToVectors received_data;

  // Post all of the sends, non-blocking and synchronous

  // Save off the old send_mode so we can restore it after this
  auto old_send_mode = comm.send_mode();

  // Set the sending to synchronous - this is so that we can know when
  // the sends are complete
  const_cast<Communicator &>(comm).send_mode(Communicator::SYNCHRONOUS);

  // The send requests
  std::list<Request> reqs;

  processor_id_type num_procs = comm.size();

  for (auto & datapair : data)
    {
      // In the case of data partitioned into more processors than we
      // have ranks, we "wrap around"
      processor_id_type destid = datapair.first % num_procs;
      auto & datum = datapair.second;

      // Just act on data if the user requested a send-to-self
      if (destid == comm.rank())
        act_on_data(destid, datum);
      else
        {
          Request sendreq;
          comm.nonblocking_send_packed_range(destid, &datum, datum.begin(), datum.end(), sendreq, tag);
          reqs.push_back(sendreq);
        }
    }

  bool sends_complete = reqs.empty();
  bool started_barrier = false;
  Request barrier_request;

  // Receive

  // The pair of src_pid and requests
  std::list<std::pair<unsigned int, std::shared_ptr<Request>>> receive_reqs;
  auto current_request = std::make_shared<Request>();

  std::multimap<processor_id_type, std::shared_ptr<std::vector<nonconst_nonref_type>>> incoming_data;
  auto current_incoming_data = std::make_shared<std::vector<nonconst_nonref_type>>();

  nonconst_nonref_type * output_type;

  unsigned int current_src_proc = 0;

  // Keep looking for receives
  while (true)
  {
    // Look for data from anywhere
    current_src_proc = TIMPI::any_source;

    // Check if there is a message and start receiving it
    if (comm.possibly_receive_packed_range(current_src_proc,
                                           receive_context,
                                           std::back_inserter(*current_incoming_data),
                                           output_type,
                                           *current_request,
                                           tag))
    {
      receive_reqs.emplace_back(current_src_proc, current_request);
      current_request = std::make_shared<Request>();

      // current_src_proc will now hold the src pid for this receive
      incoming_data.emplace(current_src_proc, current_incoming_data);
      current_incoming_data = std::make_shared<std::vector<nonconst_nonref_type>>();
    }

    // Clean up outstanding receive requests
    receive_reqs.remove_if([&act_on_data, &incoming_data](std::pair<unsigned int, std::shared_ptr<Request>> & pid_req_pair)
                           {
                             auto & pid = pid_req_pair.first;
                             auto & req = pid_req_pair.second;

                             // If it's finished - let's act on it
                             if (req->test())
                             {
                               // Do any post-wait work
                               req->wait();

                               auto it = incoming_data.find(pid);
                               timpi_assert(it != incoming_data.end());

                               act_on_data(pid, *it->second);

                               // Don't need this data anymore
                               incoming_data.erase(it);

                               // This removes it from the list
                               return true;
                             }

                             // Not finished yet
                             return false;
                           });

    reqs.remove_if([](Request & req)
                   {
                     if (req.test())
                     {
                       // Do Post-Wait work
                       req.wait();

                       return true;
                     }

                     // Not finished yet
                     return false;
                   });


    // See if all of the sends are finished
    if (reqs.empty())
      sends_complete = true;

    // If they've all completed then we can start the barrier
    if (sends_complete && !started_barrier)
    {
      started_barrier = true;
      comm.nonblocking_barrier(barrier_request);
    }

    // Must fully receive everything before being allowed to move on!
    if (receive_reqs.empty())
      // See if all proessors have finished all sends (i.e. _done_!)
      if (started_barrier)
        if (barrier_request.test())
          break; // Done!
  }

  // Reset the send mode
  const_cast<Communicator &>(comm).send_mode(old_send_mode);
}


template <typename datum,
          typename MapToVectors,
          typename GatherFunctor,
          typename ActionFunctor>
void pull_parallel_vector_data(const Communicator & comm,
                               const MapToVectors & queries,
                               GatherFunctor & gather_data,
                               ActionFunctor & act_on_data,
                               const datum *)
{
  typedef typename MapToVectors::mapped_type query_type;

  std::multimap<processor_id_type, std::vector<datum> >
    response_data, received_data;

#ifndef NDEBUG
  processor_id_type max_pid = 0;
  for (auto p : queries)
    max_pid = std::max(max_pid, p.first);
#endif

  auto gather_functor =
    [&gather_data, &response_data]
    (processor_id_type pid, query_type query)
    {
      auto new_data_it =
        response_data.emplace(pid, std::vector<datum>());
      gather_data(pid, query, new_data_it->second);
      timpi_assert_equal_to(query.size(), new_data_it->second.size());
    };

  push_parallel_vector_data (comm, queries, gather_functor);

  std::map<processor_id_type, unsigned int> responses_acted_on;

  const processor_id_type num_procs = comm.size();

  auto action_functor =
    [&act_on_data, &queries, &responses_acted_on,
#ifndef NDEBUG
     max_pid,
#endif
     num_procs
    ]
    (processor_id_type pid, const std::vector<datum> & data)
    {
      auto q_pid_its = queries.equal_range(pid);
      auto query_it = q_pid_its.first;
      timpi_assert(query_it != q_pid_its.second);

      // We rely on responses coming in the same order as queries
      const unsigned int nth_query = responses_acted_on[pid]++;
      for (unsigned int i=0; i != nth_query; ++i)
        {
          query_it++;
          if (query_it == q_pid_its.second)
            {
              do
                {
                  pid += num_procs;
                  q_pid_its = queries.equal_range(pid);
                  timpi_assert_less_equal(pid, max_pid);
                } while (q_pid_its.first == q_pid_its.second);
              query_it = q_pid_its.first;
            }
        }

      act_on_data(pid, query_it->second, data);
    };

  push_parallel_vector_data (comm, response_data, action_functor);
}




template <typename datum,
          typename A,
          typename MapToVectors,
          typename GatherFunctor,
          typename ActionFunctor>
void pull_parallel_vector_data(const Communicator & comm,
                               const MapToVectors & queries,
                               GatherFunctor & gather_data,
                               ActionFunctor & act_on_data,
                               const std::vector<datum,A> *)
{
  typedef typename MapToVectors::mapped_type query_type;

  // First index: order of creation, irrelevant
  std::vector<std::vector<std::vector<datum,A>>> response_data;
  std::vector<Request> response_reqs;

  // We'll grab a tag so we can overlap request sends and receives
  // without confusing one for the other
  MessageTag tag = comm.get_unique_tag();

  auto gather_functor =
    [&comm, &gather_data, &act_on_data,
     &response_data, &response_reqs, &tag]
    (processor_id_type pid, query_type query)
    {
      std::vector<std::vector<datum,A>> response;
      gather_data(pid, query, response);
      timpi_assert_equal_to(query.size(),
                              response.size());

      // Just act on data if the user requested a send-to-self
      if (pid == comm.rank())
        {
          act_on_data(pid, query, response);
        }
      else
        {
          Request sendreq;
          comm.send(pid, response, sendreq, tag);
          response_reqs.push_back(sendreq);
          response_data.push_back(std::move(response));
        }
    };

  push_parallel_vector_data (comm, queries, gather_functor);

  // Every outgoing query should now have an incoming response.
  //
  // Post all of the receives.
  //
  // Use blocking API here since we can't use the pre-sized
  // non-blocking APIs with this data type.
  //
  // FIXME - implement Derek's API from #1684, switch to that!
  std::vector<Request> receive_reqs;
  std::vector<processor_id_type> receive_procids;
  for (std::size_t i = 0,
       n_queries = queries.size() - queries.count(comm.rank());
       i != n_queries; ++i)
    {
      Status stat(comm.probe(any_source, tag));
      const processor_id_type
        proc_id = cast_int<processor_id_type>(stat.source());

      std::vector<std::vector<datum,A>> received_data;
      comm.receive(proc_id, received_data, tag);

      timpi_assert(queries.count(proc_id));
      auto & querydata = queries.at(proc_id);
      timpi_assert_equal_to(querydata.size(), received_data.size());
      act_on_data(proc_id, querydata, received_data);
    }

  wait(response_reqs);
}

} // namespace TIMPI

#endif // TIMPI_PARALLEL_SYNC_H
