/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2018 Natale Patriciello <natale.patriciello@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include "tcp-rate-ops.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(TcpRateOps);

TypeId
TcpRateOps::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpRateOps")
    .SetParent<Object> ()
    .SetGroupName ("Internet")
  ;
  return tid;
}

NS_LOG_COMPONENT_DEFINE ("TcpRateLinux");
NS_OBJECT_ENSURE_REGISTERED(TcpRateLinux);

TypeId
TcpRateLinux::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpRateLinux")
    .SetParent<TcpRateOps> ()
    .SetGroupName ("Internet")
    .AddTraceSource ("TcpRateUpdated",
                     "Tcp rate information has been updated",
                     MakeTraceSourceAccessor (&TcpRateLinux::m_rateTrace),
                     "ns3::TcpRateLinux::TcpRateUpdated")
    .AddTraceSource ("TcpRateSampleUpdated",
                     "Tcp rate sample has been updated",
                     MakeTraceSourceAccessor (&TcpRateLinux::m_rateSampleTrace),
                     "ns3::TcpRateLinux::TcpRateSampleUpdated")
  ;
  return tid;
}

const TcpRateOps::TcpRateSample &
TcpRateLinux::SampleGen(uint32_t delivered, uint32_t lost, bool is_sack_reneg,
                        uint32_t priorInFlight, const Time &minRtt)
{
  NS_LOG_FUNCTION (this << delivered << lost << is_sack_reneg);

  /* Clear app limited if bubble is acked and gone. */
  if (m_rate.m_appLimited != 0 && m_rate.m_delivered > m_rate.m_appLimited)
    {
      m_rate.m_appLimited = 0;
    }

  m_rateSample.m_ackedSacked = delivered;   /* freshly ACKed or SACKed */
  m_rateSample.m_bytesLoss   = lost;        /* freshly marked lost */
  m_rateSample.m_priorDelivered = priorInFlight;

  /* Return an invalid sample if no timing information is available or
   * in recovery from loss with SACK reneging. Rate samples taken during
   * a SACK reneging event may overestimate bw by including packets that
   * were SACKed before the reneg.
   */
  if (m_rateSample.m_priorTime == Seconds (0) || is_sack_reneg)
    {
      m_rateSample.m_delivered = -1;
      m_rateSample.m_interval = Seconds (0);
      m_rateSampleTrace (m_rateSample);
      return m_rateSample;
    }

  // LINUX:
  //  /* Model sending data and receiving ACKs as separate pipeline phases
  //   * for a window. Usually the ACK phase is longer, but with ACK
  //   * compression the send phase can be longer. To be safe we use the
  //   * longer phase.
  //   */
  //  auto snd_us = m_rateSample.m_interval;  /* send phase */
  //  auto ack_us = Simulator::Now () - m_rateSample.m_prior_mstamp;
  //  m_rateSample.m_interval = std::max (snd_us, ack_us);

  m_rateSample.m_interval = std::max (m_rateSample.m_sendElapsed, m_rateSample.m_ackElapsed);
  m_rateSample.m_delivered = m_rate.m_delivered - m_rateSample.m_priorDelivered;

  /* Normally we expect m_interval >= minRtt.
   * Note that rate may still be over-estimated when a spuriously
   * retransmistted skb was first (s)acked because "interval_us"
   * is under-estimated (up to an RTT). However continuously
   * measuring the delivery rate during loss recovery is crucial
   * for connections suffer heavy or prolonged losses.
   */
  if (m_rateSample.m_interval < minRtt)
    {
      m_rateSample.m_interval  = Seconds (0);
      m_rateSample.m_priorTime = Seconds (0); // To make rate sample invalid
      m_rateSampleTrace (m_rateSample);
      return m_rateSample;
    }

  /* Record the last non-app-limited or the highest app-limited bw */
  if (!m_rateSample.m_isAppLimited ||
      (m_rateSample.m_delivered * m_rate.m_rateInterval >=
       m_rate.m_rateDelivered * m_rateSample.m_interval))
    {
      m_rate.m_rateDelivered  = m_rateSample.m_delivered;
      m_rate.m_rateInterval   = m_rateSample.m_interval;
      m_rate.m_rateAppLimited = m_rateSample.m_isAppLimited;
      m_rateSample.m_deliveryRate = DataRate (m_rateSample.m_delivered * 8.0 / m_rateSample.m_interval.GetSeconds ());
    }

  m_rateSampleTrace (m_rateSample);
  return m_rateSample;
}

void
TcpRateLinux::CalculateAppLimited (uint32_t cWnd, uint32_t in_flight,
                                   uint32_t segmentSize, const SequenceNumber32 &tailSeq,
                                   const SequenceNumber32 &nextTx)
{
  NS_LOG_FUNCTION (this);

  /* Missing checks from Linux:
   * - Nothing in sending host's qdisc queues or NIC tx queue. NOT IMPLEMENTED
   * - All lost packets have been retransmitted
   * (for the above: m_txBuffer->GetLost () <= m_txBuffer->GetRetransmitsCount ())
   */
  if (tailSeq - nextTx < static_cast<int32_t> (segmentSize) && // We have less than one packet to send.
      in_flight < cWnd)                                        // We are not limited by CWND.
    {
      m_rate.m_appLimited = std::max (m_rate.m_delivered + in_flight, 1UL);
      m_rateTrace (m_rate);
    }

  // m_appLimited will be reset once in SampleGen, if it has to be.
  // else
  //  {
  //    m_rate.m_appLimited = 0U;
  //  }
}

void
TcpRateLinux::SkbDelivered (TcpTxItem * skb)
{
  NS_LOG_FUNCTION (this << skb);

  TcpTxItem::RateInformation & skbInfo = skb->GetRateInformation ();

  if (skbInfo.m_deliveredTime == Time::Max ())
    {
      return;
    }

  m_rate.m_delivered    += skb->GetSeqSize();
  m_rate.m_deliveredTime = Simulator::Now ();

  if (!m_rateSample.m_priorDelivered || skbInfo.m_delivered > m_rateSample.m_priorDelivered)
    {
      m_rateSample.m_priorDelivered   = skbInfo.m_delivered;
      m_rateSample.m_priorTime        = skbInfo.m_deliveredTime;
      m_rateSample.m_isAppLimited     = skbInfo.m_isAppLimited;
      m_rateSample.m_sendElapsed      = skb->GetLastSent () - skbInfo.m_firstSent;
      m_rateSample.m_ackElapsed       = Simulator::Now () - skbInfo.m_deliveredTime;

      m_rateSampleTrace (m_rateSample);

      m_rate.m_firstSentTime          = skb->GetLastSent ();
    }

  /* Mark off the skb delivered once it's taken into account to avoid being
   * used again when it's cumulatively acked, in case it was SACKed.
   */
  skbInfo.m_deliveredTime = Time::Max ();
  m_rate.m_txItemDelivered = skbInfo.m_delivered;
  m_rateTrace (m_rate);
}

void
TcpRateLinux::SkbSent (TcpTxItem *skb, bool isStartOfTransmission)
{
  NS_LOG_FUNCTION (this << skb << isStartOfTransmission);

  TcpTxItem::RateInformation & skbInfo = skb->GetRateInformation ();

  /* In general we need to start delivery rate samples from the
   * time we received the most recent ACK, to ensure we include
   * the full time the network needs to deliver all in-flight
   * packets. If there are no packets in flight yet, then we
   * know that any ACKs after now indicate that the network was
   * able to deliver those packets completely in the sampling
   * interval between now and the next ACK.
   *
   * Note that we use the entire window size instead of bytes_in_flight
   * because the latter is a guess based on RTO and loss-marking
   * heuristics. We don't want spurious RTOs or loss markings to cause
   * a spuriously small time interval, causing a spuriously high
   * bandwidth estimate.
   */
  if (isStartOfTransmission)
    {
      NS_LOG_INFO ("Starting of a transmission at time " << Simulator::Now().GetSeconds ());
      m_rate.m_firstSentTime = Simulator::Now ();
      m_rate.m_deliveredTime = Simulator::Now ();
      m_rateTrace (m_rate);
    }

  skbInfo.m_firstSent     = m_rate.m_firstSentTime;
  skbInfo.m_deliveredTime = m_rate.m_deliveredTime;
  skbInfo.m_isAppLimited  = (m_rate.m_appLimited != 0);
  skbInfo.m_delivered     = m_rate.m_delivered;
}


} // namespace ns3
