/** AVC & HEVC video helper functions

   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL v2
   see the file COPYING for details
   or visit https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

   \author Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#pragma once

#include "common/common_pch.h"

namespace mtx::avc_hevc {

constexpr auto NALU_START_CODE = 0x00000001;

struct slice_info_t {
public:
  // Fields common to AVC & HEVC:
  unsigned char nalu_type{};
  unsigned char slice_type{};
  unsigned char pps_id{};
  unsigned int sps{};
  unsigned int pps{};
  unsigned int pic_order_cnt_lsb{};

  // AVC-specific fields:
  unsigned char nal_ref_idc{};
  unsigned int frame_num{};
  bool field_pic_flag{}, bottom_field_flag{};
  unsigned int idr_pic_id{};
  unsigned int delta_pic_order_cnt_bottom{};
  unsigned int delta_pic_order_cnt[2]{};
  unsigned int first_mb_in_slice{};

  // HEVC-specific fields:
  bool first_slice_segment_in_pic_flag{};
  int temporal_id{};

public:
  void dump() const;
  void clear();
};

struct frame_t {
public:
  // Fields common to AVC & HEVC:
  memory_cptr m_data;
  int64_t m_start{}, m_end{}, m_ref1{}, m_ref2{};
  uint64_t m_position{};
  bool m_keyframe{}, m_has_provided_timestamp{};
  mtx::avc_hevc::slice_info_t m_si{};
  int m_presentation_order{}, m_decode_order{};

  // AVC-specific fields:
  char m_type{'?'};
  bool m_order_calculated{};

public:
  void clear();
  bool is_i_frame() const;
  bool is_p_frame() const;
  bool is_b_frame() const;
  bool is_key_frame() const;
  bool is_discardable() const;
};

}
