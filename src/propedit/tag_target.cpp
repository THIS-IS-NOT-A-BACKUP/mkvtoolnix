/*
   mkvpropedit -- utility for editing properties of existing Matroska files

   Distributed under the GPL v2
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include "common/common_pch.h"

#include <boost/date_time/posix_time/posix_time.hpp>

#include <matroska/KaxCluster.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxTag.h>
#include <matroska/KaxTags.h>
#include <matroska/KaxTracks.h>

#include "common/content_decoder.h"
#include "common/hacks.h"
#include "common/kax_analyzer.h"
#include "common/kax_file.h"
#include "common/list_utils.h"
#include "common/output.h"
#include "common/strings/editing.h"
#include "common/strings/parsing.h"
#include "common/version.h"
#include "common/xml/ebml_tags_converter.h"
#include "propedit/propedit.h"
#include "propedit/tag_target.h"

using namespace libmatroska;

tag_target_c::tag_target_c()
  : track_target_c{""}
  , m_operation_mode{tom_undefined}
{
}

tag_target_c::tag_target_c(tag_operation_mode_e operation_mode)
  : track_target_c{""}
  , m_operation_mode{operation_mode}
{
}

tag_target_c::~tag_target_c() {
}

bool
tag_target_c::operator ==(target_c const &cmp)
  const {
  auto other_tag = dynamic_cast<tag_target_c const *>(&cmp);

  return other_tag
    && (m_operation_mode       == other_tag->m_operation_mode)
    && (m_selection_mode       == other_tag->m_selection_mode)
    && (m_selection_param      == other_tag->m_selection_param)
    && (m_selection_track_type == other_tag->m_selection_track_type);
}

void
tag_target_c::validate() {
  if (mtx::included_in(m_operation_mode, tom_add_track_statistics, tom_delete_track_statistics))
    return;

  if (!m_file_name.empty() && !m_new_tags)
    m_new_tags = mtx::xml::ebml_tags_converter_c::parse_file(m_file_name, false);
}

void
tag_target_c::parse_tags_spec(const std::string &spec) {
  m_spec                         = spec;
  std::vector<std::string> parts = split(spec, ":", 2);

  if (2 != parts.size())
    throw std::runtime_error("missing file name");

  balg::to_lower(parts[0]);

  if (parts[0] == "all")
    m_operation_mode = tom_all;

  else if (parts[0] == "global")
    m_operation_mode = tom_global;

  else if (parts[0] == "track") {
    m_operation_mode = tom_track;
    parts                = split(parts[1], ":", 2);
    parse_spec(balg::to_lower_copy(parts[0]));

  } else
    throw false;

  m_file_name = parts[1];
}

void
tag_target_c::dump_info()
  const
{
  mxinfo(boost::format("  tag_target:\n"
                       "    operation_mode:       %1%\n"
                       "    selection_mode:       %2%\n"
                       "    selection_param:      %3%\n"
                       "    selection_track_type: %4%\n"
                       "    track_uid:            %5%\n"
                       "    file_name:            %6%\n")
         % static_cast<int>(m_operation_mode)
         % static_cast<int>(m_selection_mode)
         % m_selection_param
         % m_selection_track_type
         % m_track_uid
         % m_file_name);

  for (auto &change : m_changes)
    change->dump_info();
}

bool
tag_target_c::has_changes()
  const {
  return true;
}

bool
tag_target_c::non_track_target()
  const {
  return mtx::included_in(m_operation_mode, tom_all, tom_global, tom_add_track_statistics, tom_delete_track_statistics);
}

bool
tag_target_c::sub_master_is_track()
  const {
  return true;
}

bool
tag_target_c::requires_sub_master()
  const {
  return false;
}

void
tag_target_c::execute() {
  if (tom_all == m_operation_mode) {
    add_or_replace_all_master_elements(m_new_tags.get());
    m_tags_modified = true;

  } else if (tom_global == m_operation_mode)
    add_or_replace_global_tags(m_new_tags.get());

  else if (tom_track == m_operation_mode)
    add_or_replace_track_tags(m_new_tags.get());

  else if (tom_add_track_statistics == m_operation_mode)
    add_or_replace_track_statistics_tags();

  else if (tom_delete_track_statistics == m_operation_mode)
    delete_track_statistics_tags();

  else
    assert(false);

  if (!m_level1_element->ListSize())
    return;

  fix_mandatory_elements(m_level1_element);
  if (!m_level1_element->CheckMandatory())
    mxerror(boost::format(Y("Error parsing the tags in '%1%': some mandatory elements are missing.\n")) % m_file_name);

  if (m_analyzer->is_webm())
    mtx::tags::remove_elements_unsupported_by_webm(*m_level1_element);
}

void
tag_target_c::add_or_replace_global_tags(KaxTags *tags) {
  size_t idx = 0;
  while (m_level1_element->ListSize() > idx) {
    KaxTag *tag = dynamic_cast<KaxTag *>((*m_level1_element)[idx]);
    if (!tag || (-1 != mtx::tags::get_tuid(*tag)))
      ++idx;
    else {
      delete tag;
      m_level1_element->Remove(idx);
      m_tags_modified = true;
    }
  }

  if (tags) {
    idx = 0;
    while (tags->ListSize() > idx) {
      KaxTag *tag = dynamic_cast<KaxTag *>((*tags)[0]);
      if (!tag || (-1 != mtx::tags::get_tuid(*tag)))
        ++idx;
      else {
        m_level1_element->PushElement(*tag);
        tags->Remove(idx);
        m_tags_modified = true;
      }
    }
  }
}

void
tag_target_c::add_or_replace_track_tags(KaxTags *tags) {
  int64_t track_uid = GetChild<KaxTrackUID>(m_sub_master).GetValue();

  size_t idx = 0;
  while (m_level1_element->ListSize() > idx) {
    KaxTag *tag = dynamic_cast<KaxTag *>((*m_level1_element)[idx]);
    if (!tag || (track_uid != mtx::tags::get_tuid(*tag)))
      ++idx;
    else {
      delete tag;
      m_level1_element->Remove(idx);
      m_tags_modified = true;
    }
  }

  if (tags) {
    mtx::tags::remove_track_uid_targets(tags);

    idx = 0;
    while (tags->ListSize() > idx) {
      KaxTag *tag = dynamic_cast<KaxTag *>((*tags)[0]);
      if (!tag)
        ++idx;
      else {
        GetChild<KaxTagTrackUID>(GetChild<KaxTagTargets>(tag)).SetValue(track_uid);
        m_level1_element->PushElement(*tag);
        tags->Remove(idx);
        m_tags_modified = true;
      }
    }
  }
}

bool
tag_target_c::read_segment_info_and_tracks() {
  auto tracks       = m_analyzer->read_all(KaxTracks::ClassInfos);
  auto segment_info = m_analyzer->read_all(KaxInfo::ClassInfos);
  m_timestamp_scale = segment_info ? FindChildValue<KaxTimecodeScale>(*segment_info, 1000000ull) : 1000000ull;

  if (tracks && dynamic_cast<KaxTracks *>(tracks.get())) {
    for (int idx = 0, num_children = tracks->ListSize(); idx < num_children; ++idx) {
      auto track = dynamic_cast<KaxTrackEntry *>((*tracks)[idx]);

      if (!track)
        continue;

      auto track_number     = FindChildValue<KaxTrackNumber>(*track);
      auto track_uid        = FindChildValue<KaxTrackUID>(*track);
      auto default_duration = FindChildValue<KaxTrackDefaultDuration>(*track);

      m_default_durations_by_number[track_number] = default_duration;
      m_track_statistics_by_number.emplace(track_number, track_statistics_c{track_uid});
      m_content_decoders_by_number.emplace(track_number, std::shared_ptr<content_decoder_c>(new content_decoder_c{*track}));

      if (!m_content_decoders_by_number[track_number]->is_ok())
        mxerror(Y("Tracks with unsupported content encoding schemes (compression or encryption) cannot be modified.\n"));
    }
  }

  if (!m_track_statistics_by_number.empty())
    return true;

  mxwarn(Y("No track headers were found for which statistics could be calculated.\n"));
  return false;
}

void
tag_target_c::account_frame(uint64_t track_num,
                            uint64_t timestamp,
                            uint64_t duration,
                            memory_cptr frame) {
  auto &decoder = m_content_decoders_by_number[track_num];

  if (decoder)
    decoder->reverse(frame, CONTENT_ENCODING_SCOPE_BLOCK);

  m_track_statistics_by_number[track_num].account(timestamp, duration, frame->get_size());
}

void
tag_target_c::account_block_group(KaxBlockGroup &block_group,
                                  KaxCluster &cluster) {
  auto block = FindChild<KaxBlock>(block_group);
  if (!block)
    return;

  auto num_frames = block->NumberFrames();
  auto stats_itr  = m_track_statistics_by_number.find(block->TrackNum());

  if (!num_frames || (stats_itr == m_track_statistics_by_number.end()))
    return;

  block->SetParent(cluster);

  auto block_duration  = FindChild<KaxBlockDuration>(block_group);
  auto frame_duration  = block_duration ? static_cast<uint64_t>(block_duration->GetValue() * m_timestamp_scale / num_frames) : m_default_durations_by_number[block->TrackNum()];
  auto first_timestamp = block->GlobalTimecode();

  for (int idx = 0; idx < static_cast<int>(num_frames); ++idx) {
    auto &data_buffer = block->GetBuffer(idx);
    account_frame(block->TrackNum(), first_timestamp + idx * frame_duration, frame_duration, std::make_shared<memory_c>(data_buffer.Buffer(), data_buffer.Size(), false));
  }
}

void
tag_target_c::account_simple_block(KaxSimpleBlock &simple_block,
                                   KaxCluster &cluster) {
  auto num_frames = simple_block.NumberFrames();
  auto stats_itr  = m_track_statistics_by_number.find(simple_block.TrackNum());

  if (!num_frames || (stats_itr == m_track_statistics_by_number.end()))
    return;

  simple_block.SetParent(cluster);

  auto frame_duration  = m_default_durations_by_number[simple_block.TrackNum()];
  auto first_timestamp = simple_block.GlobalTimecode();

  for (int idx = 0; idx < static_cast<int>(num_frames); ++idx) {
    auto &data_buffer = simple_block.GetBuffer(idx);
    account_frame(simple_block.TrackNum(), first_timestamp + idx * frame_duration, frame_duration, std::make_shared<memory_c>(data_buffer.Buffer(), data_buffer.Size(), false));
  }
}

void
tag_target_c::account_one_cluster(KaxCluster &cluster) {
  for (int idx = 0, num_children = cluster.ListSize(); idx < num_children; ++idx) {
    auto child = cluster[idx];

    if (Is<KaxBlockGroup>(child))
      account_block_group(*static_cast<KaxBlockGroup *>(child), cluster);

    else if (Is<KaxSimpleBlock>(child))
      account_simple_block(*static_cast<KaxSimpleBlock *>(child), cluster);
  }
}

void
tag_target_c::account_all_clusters() {
  auto &file             = m_analyzer->get_file();
  auto kax_file          = std::make_shared<kax_file_c>(file);
  auto file_size         = file.get_size();
  auto previous_progress = 0;

  file.setFilePointer(m_analyzer->get_segment_data_start_pos());

  mxinfo(Y("The file is read in order to create track statistics.\n"));
  mxinfo(boost::format(Y("Progress: %1%%%%2%")) % 0 % "\r");

  while (true) {
    auto cluster = std::unique_ptr<KaxCluster>{kax_file->read_next_cluster()};
    if (!cluster)
      break;

    cluster->InitTimecode(FindChildValue<KaxClusterTimecode>(*cluster), m_timestamp_scale);

    account_one_cluster(*cluster);

    auto current_progress = std::lround(file.getFilePointer() * 100ull / static_cast<double>(file_size));
    if (current_progress != previous_progress) {
      mxinfo(boost::format(Y("Progress: %1%%%%2%")) % current_progress % "\r");
      previous_progress = current_progress;
    }
  }

  mxinfo(boost::format(Y("Progress: %1%%%%2%")) % 100 % "\n");
}

void
tag_target_c::create_track_statistics_tags() {
  auto no_variable_data = hack_engaged(ENGAGE_NO_VARIABLE_DATA);
  auto writing_app      = no_variable_data ? "no_variable_data"         : get_version_info("mkvpropedit", static_cast<version_info_flags_e>(vif_full | vif_untranslated));
  auto writing_date     = no_variable_data ? boost::posix_time::ptime{} : boost::posix_time::second_clock::universal_time();

  auto track_numbers    = std::vector<uint64_t>{};

  for (auto const &elt : m_track_statistics_by_number)
    track_numbers.push_back(elt.first);

  brng::sort(track_numbers);

  for (auto const &track_number : track_numbers)
    m_track_statistics_by_number[track_number].create_tags(*static_cast<KaxTags *>(m_level1_element), writing_app, writing_date);
}

void
tag_target_c::add_or_replace_track_statistics_tags() {
  if (!read_segment_info_and_tracks())
    return;

  delete_track_statistics_tags();

  account_all_clusters();

  create_track_statistics_tags();

  m_tags_modified = true;
}

void
tag_target_c::delete_track_statistics_tags() {
  m_tags_modified = mtx::tags::remove_track_statistics(static_cast<KaxTags *>(m_level1_element), boost::none);
}

bool
tag_target_c::has_content_been_modified()
  const {
  return m_tags_modified;
}

bool
tag_target_c::write_elements_set_to_default_value()
  const {
  return !m_analyzer->is_webm();
}

bool
tag_target_c::add_mandatory_elements_if_missing()
  const {
  return false;
}
