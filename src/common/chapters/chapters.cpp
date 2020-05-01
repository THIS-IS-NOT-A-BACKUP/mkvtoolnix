/** \brief chapter parser and helper functions

   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL v2
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   \file

   \author Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include "common/common_pch.h"

#include <algorithm>
#include <cassert>

#include <matroska/KaxChapters.h>

#include "common/chapters/chapters.h"
#include "common/ebml.h"
#include "common/error.h"
#include "common/extern_data.h"
#include "common/locale.h"
#include "common/mm_io_x.h"
#include "common/mm_file_io.h"
#include "common/mm_proxy_io.h"
#include "common/mm_text_io.h"
#include "common/strings/editing.h"
#include "common/strings/formatting.h"
#include "common/strings/parsing.h"
#include "common/strings/regex.h"
#include "common/unique_numbers.h"
#include "common/xml/ebml_chapters_converter.h"

using namespace libmatroska;

namespace mtx::chapters {

/** The default language for all chapter entries that don't have their own. */
std::string g_default_language;
/** The default country for all chapter entries that don't have their own. */
std::string g_default_country;

translatable_string_c g_chapter_generation_name_template{YT("Chapter <NUM:2>")};

#define SIMCHAP_RE_TIMESTAMP_LINE "^\\s*CHAPTER\\d+\\s*=\\s*(\\d+)\\s*:\\s*(\\d+)\\s*:\\s*(\\d+)\\s*[\\.,]\\s*(\\d+)"
#define SIMCHAP_RE_TIMESTAMP      "^\\s*CHAPTER\\d+\\s*=(.*)"
#define SIMCHAP_RE_NAME_LINE      "^\\s*CHAPTER\\d+NAME\\s*=(.*)"

/** \brief Throw a special chapter parser exception.

   \param error The error message.
*/
inline void
chapter_error(const std::string &error) {
  throw parser_x(fmt::format(Y("Simple chapter parser: {0}\n"), error));
}

/** \brief Reads the start of a file and checks for OGM style comments.

   The first lines are read. OGM style comments are recognized if the first
   non-empty line contains <tt>CHAPTER01=...</tt> and the first non-empty
   line afterwards contains <tt>CHAPTER01NAME=...</tt>.

   The parameters are checked for validity.

   \param in The file to read from.

   \return \c true if the file contains OGM style comments and \c false
     otherwise.
*/
bool
probe_simple(mm_text_io_c *in) {
  std::regex timestamp_line_re{SIMCHAP_RE_TIMESTAMP_LINE};
  std::regex name_line_re{     SIMCHAP_RE_NAME_LINE};
  std::smatch matches;

  std::string line;

  assert(in);

  in->setFilePointer(0);
  while (in->getline2(line)) {
    strip(line);
    if (line.empty())
      continue;

    if (!std::regex_search(line, timestamp_line_re))
      return false;

    while (in->getline2(line)) {
      strip(line);
      if (line.empty())
        continue;

      return std::regex_search(line, name_line_re);
    }

    return false;
  }

  return false;
}

//           1         2
// 012345678901234567890123
//
// CHAPTER01=00:00:00.000
// CHAPTER01NAME=Hallo Welt

/** \brief Parse simple OGM style comments

   The file \a in is read. The content is assumed to be OGM style comments.

   The parameters are checked for validity.

   \param in The text file to read from.
   \param min_ts An optional timestamp. If both \a min_ts and \a max_ts are
     given then only those chapters that lie in the timerange
     <tt>[min_ts..max_ts]</tt> are kept.
   \param max_ts An optional timestamp. If both \a min_ts and \a max_ts are
     given then only those chapters that lie in the timerange
     <tt>[min_ts..max_ts]</tt> are kept.
   \param offset An optional offset that is subtracted from all start and
     end timestamps after the timerange check has been made.
   \param language This language is added as the \c KaxChapterLanguage
     for all entries.
   \param charset The charset the chapters are supposed to be it. The entries
     will be converted to UTF-8 if necessary.
   \param exception_on_error If set to \c true then an exception is thrown
     if an error occurs. Otherwise \c nullptr will be returned.

   \return The chapters parsed from the file or \c nullptr if an error occurred.
*/
kax_cptr
parse_simple(mm_text_io_c *in,
             int64_t min_ts,
             int64_t max_ts,
             int64_t offset,
             std::string const &language,
             std::string const &charset) {
  assert(in);

  in->setFilePointer(0);

  kax_cptr chaps{new KaxChapters};
  KaxChapterAtom *atom     = nullptr;
  KaxEditionEntry *edition = nullptr;
  int mode                 = 0;
  int num                  = 0;
  int64_t start            = 0;
  charset_converter_cptr cc_utf8;

  // The core now uses ns precision timestamps.
  if (0 < min_ts)
    min_ts /= 1000000;
  if (0 < max_ts)
    max_ts /= 1000000;
  if (0 < offset)
    offset /= 1000000;

  bool do_convert = in->get_byte_order_mark() == byte_order_mark_e::none;
  if (do_convert)
    cc_utf8 = charset_converter_c::init(charset);

  std::string use_language = !language.empty()                          ? language
                           : !g_default_language.empty() ? g_default_language
                           :                                              "eng";

  std::regex timestamp_line_re{SIMCHAP_RE_TIMESTAMP_LINE};
  std::regex timestamp_re{     SIMCHAP_RE_TIMESTAMP};
  std::regex name_line_re{     SIMCHAP_RE_NAME_LINE};
  std::smatch matches;

  std::string line;

  while (in->getline2(line)) {
    strip(line);
    if (line.empty())
      continue;

    if (0 == mode) {
      if (!std::regex_match(line, matches, timestamp_line_re))
        chapter_error(fmt::format(Y("'{0}' is not a CHAPTERxx=... line."), line));

      int64_t hour = 0, minute = 0, second = 0, msecs = 0;
      parse_number(matches[1].str(), hour);
      parse_number(matches[2].str(), minute);
      parse_number(matches[3].str(), second);
      parse_number(matches[4].str(), msecs);

      if (59 < minute)
        chapter_error(fmt::format(Y("Invalid minute: {0}"), minute));
      if (59 < second)
        chapter_error(fmt::format(Y("Invalid second: {0}"), second));

      start = msecs + second * 1000 + minute * 1000 * 60 + hour * 1000 * 60 * 60;
      mode  = 1;

      if (!std::regex_match(line, matches, timestamp_re))
        chapter_error(fmt::format(Y("'{0}' is not a CHAPTERxx=... line."), line));

    } else {
      if (!std::regex_match(line, matches, name_line_re))
        chapter_error(fmt::format(Y("'{0}' is not a CHAPTERxxNAME=... line."), line));

      std::string name = matches[1].str();
      if (name.empty())
        name = format_name_template(g_chapter_generation_name_template.get_translated(), num + 1, timestamp_c::ms(start));

      mode = 0;

      if ((start >= min_ts) && ((start <= max_ts) || (max_ts == -1))) {
        if (!edition)
          edition = &GetChild<KaxEditionEntry>(*chaps);

        atom = &GetFirstOrNextChild<KaxChapterAtom>(*edition, atom);
        GetChild<KaxChapterUID>(*atom).SetValue(create_unique_number(UNIQUE_CHAPTER_IDS));
        GetChild<KaxChapterTimeStart>(*atom).SetValue((start - offset) * 1000000);

        auto &display = GetChild<KaxChapterDisplay>(*atom);

        GetChild<KaxChapterString>(display).SetValueUTF8(do_convert ? cc_utf8->utf8(name) : name);
        GetChild<KaxChapterLanguage>(display).SetValue(use_language);

        if (!g_default_country.empty())
          GetChild<KaxChapterCountry>(display).SetValue(g_default_country);

        ++num;
      }
    }
  }

  return 0 == num ? kax_cptr{} : chaps;
}

/** \brief Probe a file for different chapter formats and parse the file.

   The file \a file_name is opened and checked for supported chapter formats.
   These include simple OGM style chapters, cue sheets and mkvtoolnix' own
   XML chapter format.

   Its parameters don't have to be checked for validity.

   \param file_name The name of the text file to read from.
   \param min_ts An optional timestamp. If both \a min_ts and \a max_ts are
     given then only those chapters that lie in the timerange
     <tt>[min_ts..max_ts]</tt> are kept.
   \param max_ts An optional timestamp. If both \a min_ts and \a max_ts are
     given then only those chapters that lie in the timerange
     <tt>[min_ts..max_ts]</tt> are kept.
   \param offset An optional offset that is subtracted from all start and
     end timestamps after the timerange check has been made.
   \param language This language is added as the \c KaxChapterLanguage
     for entries that don't specifiy it.
   \param charset The charset the chapters are supposed to be it. The entries
     will be converted to UTF-8 if necessary. This parameter is ignored for XML
     chapter files.
   \param exception_on_error If set to \c true then an exception is thrown
     if an error occurs. Otherwise \c nullptr will be returned.
   \param format If given, this parameter will be set to the recognized chapter
     format. May be \c nullptr if the caller is not interested in the result.
   \param tags When parsing a cue sheet tags will be created along with the
     chapter entries. These tags will be stored in this parameter.

   \return The chapters parsed from the file or \c nullptr if an error occurred.

   \see ::parse_chapters(mm_text_io_c *in,int64_t min_ts,int64_t max_ts, int64_t offset,const std::string &language,const std::string &charset,bool exception_on_error,format_e *format,KaxTags **tags)
*/
kax_cptr
parse(const std::string &file_name,
      int64_t min_ts,
      int64_t max_ts,
      int64_t offset,
      const std::string &language,
      const std::string &charset,
      bool exception_on_error,
      format_e *format,
      std::unique_ptr<KaxTags> *tags) {
  try {
    mm_text_io_c in(std::make_shared<mm_file_io_c>(file_name));
    return parse(&in, min_ts, max_ts, offset, language, charset, exception_on_error, format, tags);

  } catch (parser_x &e) {
    if (exception_on_error)
      throw;
    mxerror(fmt::format(Y("Could not parse the chapters in '{0}': {1}\n"), file_name, e.error()));

  } catch (...) {
    if (exception_on_error)
      throw parser_x(fmt::format(Y("Could not open '{0}' for reading.\n"), file_name));
    else
      mxerror(fmt::format(Y("Could not open '{0}' for reading.\n"), file_name));
  }

  return {};
}

/** \brief Probe a file for different chapter formats and parse the file.

   The file \a in is checked for supported chapter formats. These include
   simple OGM style chapters, cue sheets and mkvtoolnix' own XML chapter
   format.

   The parameters are checked for validity.

   \param in The text file to read from.
   \param min_ts An optional timestamp. If both \a min_ts and \a max_ts are
     given then only those chapters that lie in the timerange
     <tt>[min_ts..max_ts]</tt> are kept.
   \param max_ts An optional timestamp. If both \a min_ts and \a max_ts are
     given then only those chapters that lie in the timerange
     <tt>[min_ts..max_ts]</tt> are kept.
   \param offset An optional offset that is subtracted from all start and
     end timestamps after the timerange check has been made.
   \param language This language is added as the \c KaxChapterLanguage
     for entries that don't specifiy it.
   \param charset The charset the chapters are supposed to be it. The entries
     will be converted to UTF-8 if necessary. This parameter is ignored for XML
     chapter files.
   \param exception_on_error If set to \c true then an exception is thrown
     if an error occurs. Otherwise \c nullptr will be returned.
   \param format If given, this parameter will be set to the recognized chapter
     format. May be \c nullptr if the caller is not interested in the result.
   \param tags When parsing a cue sheet tags will be created along with the
     chapter entries. These tags will be stored in this parameter.

   \return The chapters parsed from the file or \c nullptr if an error occurred.

   \see ::parse_chapters(const std::string &file_name,int64_t min_ts,int64_t max_ts, int64_t offset,const std::string &language,const std::string &charset,bool exception_on_error,format_e *format,std::unique_ptr<KaxTags> *tags)
*/
kax_cptr
parse(mm_text_io_c *in,
      int64_t min_ts,
      int64_t max_ts,
      int64_t offset,
      const std::string &language,
      const std::string &charset,
      bool exception_on_error,
      format_e *format,
      std::unique_ptr<KaxTags> *tags) {
  assert(in);

  std::string error;

  try {
    if (probe_simple(in)) {
      if (format)
        *format = format_e::ogg;
      return parse_simple(in, min_ts, max_ts, offset, language, charset);

    } else if (probe_cue(in)) {
      if (format)
        *format = format_e::cue;
      return parse_cue(in, min_ts, max_ts, offset, language, charset, tags);

    } else if (format)
      *format = format_e::xml;

    if (mtx::xml::ebml_chapters_converter_c::probe_file(in->get_file_name())) {
      auto chapters = mtx::xml::ebml_chapters_converter_c::parse_file(in->get_file_name(), true);
      return select_in_timeframe(chapters.get(), min_ts, max_ts, offset) ? chapters : nullptr;
    }

    error = fmt::format(Y("Unknown chapter file format in '{0}'. It does not contain a supported chapter format.\n"), in->get_file_name());
  } catch (mtx::chapters::parser_x &e) {
    error = e.error();
  } catch (mtx::mm_io::exception &ex) {
    error = fmt::format(Y("The XML chapter file '{0}' could not be read.\n"), in->get_file_name());
  } catch (mtx::xml::xml_parser_x &ex) {
    error = fmt::format(Y("The XML chapter file '{0}' contains an error at position {2}: {1}\n"), in->get_file_name(), ex.result().description(), ex.result().offset);
  } catch (mtx::xml::exception &ex) {
    error = fmt::format(Y("The XML chapter file '{0}' contains an error: {1}\n"), in->get_file_name(), ex.what());
  }

  if (!error.empty()) {
    if (exception_on_error)
      throw mtx::chapters::parser_x(error);
    mxerror(error);
  }

  return {};
}

/** \brief Get the start timestamp for a chapter atom.

   Its parameters don't have to be checked for validity.

   \param atom The atom for which the start timestamp should be returned.
   \param value_if_not_found The value to return if no start timestamp child
     element was found. Defaults to -1.

   \return The start timestamp or \c value_if_not_found if the atom doesn't
     contain such a child element.
*/
int64_t
get_start(KaxChapterAtom &atom,
          int64_t value_if_not_found) {
  auto start = FindChild<KaxChapterTimeStart>(&atom);

  return !start ? value_if_not_found : static_cast<int64_t>(start->GetValue());
}

/** \brief Get the end timestamp for a chapter atom.

   Its parameters don't have to be checked for validity.

   \param atom The atom for which the end timestamp should be returned.
   \param value_if_not_found The value to return if no end timestamp child
     element was found. Defaults to -1.

   \return The start timestamp or \c value_if_not_found if the atom doesn't
     contain such a child element.
*/
int64_t
get_end(KaxChapterAtom &atom,
        int64_t value_if_not_found) {
  auto end = FindChild<KaxChapterTimeEnd>(&atom);

  return !end ? value_if_not_found : static_cast<int64_t>(end->GetValue());
}

/** \brief Get the name for a chapter atom.

   Its parameters don't have to be checked for validity.

   \param atom The atom for which the name should be returned.

   \return The atom's name UTF-8 coded or \c "" if the atom doesn't contain
     such a child element.
*/
std::string
get_name(KaxChapterAtom &atom) {
  auto display = FindChild<KaxChapterDisplay>(&atom);
  if (!display)
    return "";

  auto name = FindChild<KaxChapterString>(display);
  if (!name)
    return "";

  return name->GetValueUTF8();
}

/** \brief Get the unique ID for a chapter atom.

   Its parameters don't have to be checked for validity.

   \param atom The atom for which the unique ID should be returned.

   \return The ID or \c -1 if the atom doesn't contain such a
     child element.
*/
int64_t
get_uid(KaxChapterAtom &atom) {
  auto uid = FindChild<KaxChapterUID>(&atom);

  return !uid ? -1 : static_cast<int64_t>(uid->GetValue());
}

void
remove_elements_unsupported_by_webm(EbmlMaster &master) {
  static std::unordered_map<uint32_t, bool> s_supported_elements, s_readd_with_defaults;

  if (s_supported_elements.empty()) {
#define add(ref) s_supported_elements[ EBML_ID_VALUE(EBML_ID(ref)) ] = true;
    add(KaxChapters);
    add(KaxEditionEntry);
    add(KaxChapterAtom);
    add(KaxChapterUID);
    add(KaxChapterStringUID);
    add(KaxChapterTimeStart);
    add(KaxChapterTimeEnd);
    add(KaxChapterDisplay);
    add(KaxChapterString);
    add(KaxChapterLanguage);
    add(KaxChapterCountry);
#undef add

#define add(ref) s_readd_with_defaults[ EBML_ID_VALUE(EBML_ID(ref)) ] = true;
    add(KaxEditionFlagDefault);
    add(KaxEditionFlagHidden);
    add(KaxChapterFlagEnabled);
    add(KaxChapterFlagHidden);
#undef add
  }

  auto idx = 0u;

  while (idx < master.ListSize()) {
    auto e = master[idx];

    if (e && s_supported_elements[ EBML_ID_VALUE(EbmlId(*e)) ]) {
      auto sub_master = dynamic_cast<EbmlMaster *>(e);
      if (sub_master)
        remove_elements_unsupported_by_webm(*sub_master);

      ++idx;

      continue;
    }

    if (e && s_readd_with_defaults[ EBML_ID_VALUE(EbmlId(*e)) ]) {
      auto new_with_defaults = &(e->CreateElement());
      delete e;
      master.GetElementList()[idx] = new_with_defaults;

      ++idx;

      continue;
    }

    delete e;
    master.Remove(idx);
  }
}

/** \brief Remove all chapter atoms that are outside of a time range

   All chapter atoms that lie completely outside the timestamp range
   given with <tt>[min_ts..max_ts]</tt> are deleted. This is the workhorse
   for ::select_chapters_in_timeframe

   Chapters which start before the window but end inside or after the window
   are kept as well, and their start timestamp is adjusted.

   Its parameters don't have to be checked for validity.

   \param min_ts The minimum timestamp to accept.
   \param max_ts The maximum timestamp to accept.
   \param offset This value is subtracted from both the start and end timestamp
     for each chapter after the decision whether or not to keep it has been
     made.
   \param m The master containing the elements to check.
*/
static void
remove_entries(int64_t min_ts,
               int64_t max_ts,
               int64_t offset,
               EbmlMaster &m) {
  if (0 == m.ListSize())
    return;

  struct chapter_entry_t {
    bool remove{}, spans{}, is_atom{};
    int64_t start{}, end{-1};
  };
  std::vector<chapter_entry_t> entries;
  entries.resize(m.ListSize());

  unsigned int last_atom_at = 0;
  bool last_atom_found      = false;

  // Determine whether or not an entry has to be removed. Also retrieve
  // the start and end timestamps.
  size_t i;
  for (i = 0; m.ListSize() > i; ++i) {
    auto atom = dynamic_cast<KaxChapterAtom *>(m[i]);
    if (!atom)
      continue;

    last_atom_at       = i;
    last_atom_found    = true;
    entries[i].is_atom = true;

    auto cts = static_cast<KaxChapterTimeStart *>(atom->FindFirstElt(EBML_INFO(KaxChapterTimeStart), false));

    if (cts)
      entries[i].start = cts->GetValue();

    auto cte = static_cast<KaxChapterTimeEnd *>(atom->FindFirstElt(EBML_INFO(KaxChapterTimeEnd), false));

    if (cte)
      entries[i].end = cte->GetValue();
  }

  // We can return if we don't have a single atom to work with.
  if (!last_atom_found)
    return;

  for (i = 0; m.ListSize() > i; ++i) {
    auto atom = dynamic_cast<KaxChapterAtom *>(m[i]);
    if (!atom)
      continue;

    // Calculate the end timestamps and determine whether or not an entry spans
    // several segments.
    if (-1 == entries[i].end) {
      if (i == last_atom_at)
        entries[i].end = 1LL << 62;

      else {
        int next_atom = i + 1;

        while (!entries[next_atom].is_atom)
          ++next_atom;

        entries[i].end = entries[next_atom].start;
      }
    }

    if (   (entries[i].start < min_ts)
        || ((max_ts >= 0) && (entries[i].start > max_ts)))
      entries[i].remove = true;

    if (entries[i].remove && (entries[i].start < min_ts) && (entries[i].end > min_ts))
      entries[i].spans = true;

    mxverb(3,
           fmt::format("remove_chapters: entries[{0}]: remove {1} spans {2} start {3} end {4}\n",
                       i, entries[i].remove, entries[i].spans, entries[i].start, entries[i].end));

    // Spanning entries must be kept, and their start timestamp must be
    // adjusted. Entries that are to be deleted will be deleted later and
    // have to be skipped for now.
    if (entries[i].remove && !entries[i].spans)
      continue;

    auto cts = static_cast<KaxChapterTimeStart *>(atom->FindFirstElt(EBML_INFO(KaxChapterTimeStart), false));
    auto cte = static_cast<KaxChapterTimeEnd *>(atom->FindFirstElt(EBML_INFO(KaxChapterTimeEnd), false));

    if (entries[i].spans)
      cts->SetValue(min_ts);

    cts->SetValue(cts->GetValue() - offset);

    if (cte) {
      int64_t end_ts = cte->GetValue();

      if ((max_ts >= 0) && (end_ts > max_ts))
        end_ts = max_ts;
      end_ts -= offset;

      cte->SetValue(end_ts);
    }

    auto m2 = dynamic_cast<EbmlMaster *>(m[i]);
    if (m2)
      remove_entries(min_ts, max_ts, offset, *m2);
  }

  // Now really delete those entries.
  i = m.ListSize();
  while (0 < i) {
    --i;
    if (entries[i].remove && !entries[i].spans) {
      delete m[i];
      m.Remove(i);
    }
  }
}

/** \brief Merge all chapter atoms sharing the same UID

   If two or more chapters with the same UID are encountered on the same
   level then those are merged into a single chapter. The start timestamp
   is the minimum start timestamp of all the chapters, and the end timestamp
   is the maximum end timestamp of all the chapters.

   The parameters do not have to be checked for validity.

   \param master The master containing the elements to check.
*/
void
merge_entries(EbmlMaster &master) {
  size_t master_idx;

  // Iterate over all children of the atomaster.
  for (master_idx = 0; master.ListSize() > master_idx; ++master_idx) {
    // Not every child is a chapter atomaster. Skip those.
    auto atom = dynamic_cast<KaxChapterAtom *>(master[master_idx]);
    if (!atom)
      continue;

    int64_t uid = get_uid(*atom);
    if (-1 == uid)
      continue;

    // First get the start and end time, if present.
    int64_t start_ts = get_start(*atom, 0);
    int64_t end_ts   = get_end(*atom);

    mxverb(3, fmt::format("chapters: merge_entries: looking for {0} with {1}, {2}\n", uid, start_ts, end_ts));

    // Now iterate over all remaining atoms and find those with the same
    // UID.
    size_t merge_idx = master_idx + 1;
    while (true) {
      KaxChapterAtom *merge_this = nullptr;
      for (; master.ListSize() > merge_idx; ++merge_idx) {
        auto cmp_atom = dynamic_cast<KaxChapterAtom *>(master[merge_idx]);
        if (!cmp_atom)
          continue;

        if (get_uid(*cmp_atom) == uid) {
          merge_this = cmp_atom;
          break;
        }
      }

      // If we haven't found an atom with the same UID then we're done here.
      if (!merge_this)
        break;

      // Do the merger! First get the start and end timestamps if present.
      int64_t merge_start_ts = get_start(*merge_this, 0);
      int64_t merge_end_ts   = get_end(*merge_this);

      // Then compare them to the ones we have for the soon-to-be merged
      // chapter and assign accordingly.
      if (merge_start_ts < start_ts)
        start_ts = merge_start_ts;

      if ((-1 == end_ts) || (merge_end_ts > end_ts))
        end_ts = merge_end_ts;

      // Move all chapter atoms from the merged entry into the target
      // entry so that they will be merged recursively as well.
      auto merge_child_idx = 0u;
      auto num_children    = merge_this->ListSize();

      while (merge_child_idx < num_children) {
        if (Is<KaxChapterAtom>((*merge_this)[merge_child_idx])) {
          atom->PushElement(*(*merge_this)[merge_child_idx]);
          merge_this->Remove(merge_child_idx);
          --num_children;

        } else
          ++merge_child_idx;
      }

      mxverb(3, fmt::format("chapters: merge_entries:   found one at {0} with {1}, {2}; merged to {3}, {4}\n", merge_idx, merge_start_ts, merge_end_ts, start_ts, end_ts));

      // Finally remove the entry itself.
      delete master[merge_idx];
      master.Remove(merge_idx);
    }

    // Assign the start and end timestamp to the chapter. Only assign an
    // end timestamp if one was present in at least one of the merged
    // chapter atoms.
    GetChild<KaxChapterTimeStart>(*atom).SetValue(start_ts);
    if (-1 != end_ts)
      GetChild<KaxChapterTimeEnd>(*atom).SetValue(end_ts);
  }

  // Recusively merge atoms.
  for (master_idx = 0; master.ListSize() > master_idx; ++master_idx) {
    auto merge_master = dynamic_cast<EbmlMaster *>(master[master_idx]);
    if (merge_master)
      merge_entries(*merge_master);
  }
}

/** \brief Remove all chapter atoms that are outside of a time range

   All chapter atoms that lie completely outside the timestamp range
   given with <tt>[min_ts..max_ts]</tt> are deleted.

   Chapters which start before the window but end inside or after the window
   are kept as well, and their start timestamp is adjusted.

   If two or more chapters with the same UID are encountered on the same
   level then those are merged into a single chapter. The start timestamp
   is the minimum start timestamp of all the chapters, and the end timestamp
   is the maximum end timestamp of all the chapters.

   The parameters are checked for validity.

   \param chapters The chapters to check.
   \param min_ts The minimum timestamp to accept.
   \param max_ts The maximum timestamp to accept.
   \param offset This value is subtracted from both the start and end timestamp
     for each chapter after the decision whether or not to keep it has been
     made.

   \return \c false if all chapters were discarded, \c true otherwise
*/
bool
select_in_timeframe(KaxChapters *chapters,
                    int64_t min_ts,
                    int64_t max_ts,
                    int64_t offset) {
  // Check the parameters.
  if (!chapters)
    return false;

  // Remove the atoms that are outside of the requested range.
  size_t master_idx;
  for (master_idx = 0; chapters->ListSize() > master_idx; master_idx++) {
    EbmlMaster *work_master = dynamic_cast<KaxEditionEntry *>((*chapters)[master_idx]);
    if (work_master)
      remove_entries(min_ts, max_ts, offset, *work_master);
  }

  // Count the number of atoms in each edition. Delete editions without
  // any atom in them.
  master_idx = 0;
  while (chapters->ListSize() > master_idx) {
    auto eentry = dynamic_cast<KaxEditionEntry *>((*chapters)[master_idx]);
    if (!eentry) {
      master_idx++;
      continue;
    }

    size_t num_atoms = 0, eentry_idx;
    for (eentry_idx = 0; eentry->ListSize() > eentry_idx; eentry_idx++)
      if (dynamic_cast<KaxChapterAtom *>((*eentry)[eentry_idx]))
        num_atoms++;

    if (0 == num_atoms) {
      chapters->Remove(master_idx);
      delete eentry;

    } else
      master_idx++;
  }

  return chapters->ListSize() > 0;
}

/** \brief Find an edition with a specific UID.

   Its parameters don't have to be checked for validity.

   \param chapters The chapters in which to look for the edition.
   \param uid The requested unique edition ID. The special value \c 0
     results in the first edition being returned.

   \return A pointer to the edition or \c nullptr if none has been found.
*/
KaxEditionEntry *
find_edition_with_uid(KaxChapters &chapters,
                      uint64_t uid) {
  if (0 == uid)
    return FindChild<KaxEditionEntry>(&chapters);

  size_t eentry_idx;
  for (eentry_idx = 0; chapters.ListSize() > eentry_idx; eentry_idx++) {
    auto eentry = dynamic_cast<KaxEditionEntry *>(chapters[eentry_idx]);
    if (!eentry)
      continue;

    auto euid = FindChild<KaxEditionUID>(eentry);
    if (euid && (euid->GetValue() == uid))
      return eentry;
  }

  return nullptr;
}

/** \brief Find a chapter atom with a specific UID.

   Its parameters don't have to be checked for validity.

   \param chapters The chapters in which to look for the atom.
   \param uid The requested unique atom ID. The special value \c 0 results in
     the first atom in the first edition being returned.

   \return A pointer to the atom or \c nullptr if none has been found.
*/
KaxChapterAtom *
find_chapter_with_uid(KaxChapters &chapters,
                      uint64_t uid) {
  if (0 == uid) {
    auto eentry = FindChild<KaxEditionEntry>(&chapters);
    if (!eentry)
      return nullptr;
    return FindChild<KaxChapterAtom>(eentry);
  }

  size_t eentry_idx;
  for (eentry_idx = 0; chapters.ListSize() > eentry_idx; eentry_idx++) {
    auto eentry = dynamic_cast<KaxEditionEntry *>(chapters[eentry_idx]);
    if (!eentry)
      continue;

    size_t atom_idx;
    for (atom_idx = 0; eentry->ListSize() > atom_idx; atom_idx++) {
      auto atom = dynamic_cast<KaxChapterAtom *>((*eentry)[atom_idx]);
      if (!atom)
        continue;

      auto cuid = FindChild<KaxChapterUID>(atom);
      if (cuid && (cuid->GetValue() == uid))
        return atom;
    }
  }

  return nullptr;
}

/** \brief Move all chapter atoms to another container keeping editions intact

   This function moves all chapter atoms from \a src to \a dst.
   If there's already an edition in \a dst with the same UID as the current
   one in \a src, then all atoms will be put into that edition. Otherwise
   the complete edition will simply be moved over.

   After processing \a src will be empty.

   Its parameters don't have to be checked for validity.

   \param dst The container the atoms and editions will be put into.
   \param src The container the atoms and editions will be taken from.
*/
void
move_by_edition(KaxChapters &dst,
                KaxChapters &src) {
  size_t src_idx;
  for (src_idx = 0; src.ListSize() > src_idx; src_idx++) {
    auto m = dynamic_cast<EbmlMaster *>(src[src_idx]);
    if (!m)
      continue;

    // Find an edition to which these atoms will be added.
    KaxEditionEntry *ee_dst = nullptr;
    auto euid_src = FindChild<KaxEditionUID>(m);
    if (euid_src)
      ee_dst = find_edition_with_uid(dst, euid_src->GetValue());

    // No edition with the same UID found as the one we want to handle?
    // Then simply move the complete edition over.
    if (!ee_dst)
      dst.PushElement(*m);
    else {
      // Move all atoms from the old edition to the new one.
      size_t master_idx;
      for (master_idx = 0; m->ListSize() > master_idx; master_idx++)
        if (Is<KaxChapterAtom>((*m)[master_idx]))
          ee_dst->PushElement(*(*m)[master_idx]);
        else
          delete (*m)[master_idx];

      m->RemoveAll();
      delete m;
    }
  }

  src.RemoveAll();
}

/** \brief Adjust all start and end timestamps by an offset

   All start and end timestamps are adjusted by an offset. This is done
   recursively.

   Its parameters don't have to be checked for validity.

   \param master A master containint the elements to adjust. This can be
     a KaxChapters, KaxEditionEntry or KaxChapterAtom object.
   \param offset The offset to add to each timestamp. Can be negative. If
     the resulting timestamp would be smaller than zero then it will be set
     to zero.
*/
void
adjust_timestamps(EbmlMaster &master,
                  int64_t offset,
                  int64_t numerator,
                  int64_t denominator) {
  size_t master_idx;
  for (master_idx = 0; master.ListSize() > master_idx; master_idx++) {
    if (!Is<KaxChapterAtom>(master[master_idx]))
      continue;

    auto atom  = static_cast<KaxChapterAtom *>(master[master_idx]);
    auto start = FindChild<KaxChapterTimeStart>(atom);
    auto end   = FindChild<KaxChapterTimeEnd>(atom);

    if (start)
      start->SetValue(std::max<int64_t>(static_cast<int64_t>((start->GetValue()) * numerator / denominator) + offset, 0));

    if (end)
      end->SetValue(std::max<int64_t>(static_cast<int64_t>((end->GetValue()) * numerator / denominator) + offset, 0));
  }

  for (master_idx = 0; master.ListSize() > master_idx; master_idx++) {
    auto work_master = dynamic_cast<EbmlMaster *>(master[master_idx]);
    if (work_master)
      adjust_timestamps(*work_master, offset, numerator, denominator);
  }
}

static int
count_atoms_recursively(EbmlMaster &master,
                        int count) {
  size_t master_idx;

  for (master_idx = 0; master.ListSize() > master_idx; ++master_idx)
    if (Is<KaxChapterAtom>(master[master_idx]))
      ++count;

    else if (dynamic_cast<EbmlMaster *>(master[master_idx]))
      count = count_atoms_recursively(*static_cast<EbmlMaster *>(master[master_idx]), count);

  return count;
}

int
count_atoms(EbmlMaster &master) {
  return count_atoms_recursively(master, 0);
}

/** \brief Change the chapter edition UIDs to a single value

   This function changes the UIDs of all editions for which the
   function is called to a single value. This is intended for chapters
   read from source files which do not provide their own edition UIDs
   (e.g. MP4 or OGM files) so that their chapters can be appended and
   don't end up in separate editions.

   \c chapters may be nullptr in which case nothing is done.

   \param dst chapters The chapter structure for which all edition
      UIDs will be changed.
*/
void
align_uids(KaxChapters *chapters) {
  if (!chapters)
    return;

  static uint64_t s_shared_edition_uid = 0;

  if (0 == s_shared_edition_uid)
    s_shared_edition_uid = create_unique_number(UNIQUE_CHAPTER_IDS);

  size_t idx;
  for (idx = 0; chapters->ListSize() > idx; ++idx) {
    auto edition_entry = dynamic_cast<KaxEditionEntry *>((*chapters)[idx]);
    if (!edition_entry)
      continue;

    GetChild<KaxEditionUID>(*edition_entry).SetValue(s_shared_edition_uid);
  }
}

void
align_uids(KaxChapters &reference,
           KaxChapters &modify) {
  size_t reference_idx = 0, modify_idx = 0;

  while (true) {
    KaxEditionEntry *ee_reference = nullptr;;
    while ((reference.ListSize() > reference_idx) && !(ee_reference = dynamic_cast<KaxEditionEntry *>(reference[reference_idx])))
      ++reference_idx;

    if (!ee_reference)
      return;

    KaxEditionEntry *ee_modify = nullptr;;
    while ((modify.ListSize() > modify_idx) && !(ee_modify = dynamic_cast<KaxEditionEntry *>(modify[modify_idx])))
      ++modify_idx;

    if (!ee_modify)
      return;

    GetChild<KaxEditionUID>(*ee_modify).SetValue(GetChild<KaxEditionUID>(*ee_reference).GetValue());
    ++reference_idx;
    ++modify_idx;
  }
}

static void
regenerate_uids_worker(EbmlMaster &master,
                       std::unordered_map<uint64_t, uint64_t> &new_chapter_uids) {
  for (int idx = 0, end = master.ListSize(); end > idx; ++idx) {
    auto element     = master[idx];
    auto edition_uid = dynamic_cast<KaxEditionUID *>(element);

    if (edition_uid) {
      edition_uid->SetValue(create_unique_number(UNIQUE_EDITION_IDS));
      continue;
    }

    auto chapter_uid = dynamic_cast<KaxChapterUID *>(element);

    if (chapter_uid) {
      new_chapter_uids[chapter_uid->GetValue()] = create_unique_number(UNIQUE_CHAPTER_IDS);
      chapter_uid->SetValue(new_chapter_uids[chapter_uid->GetValue()]);
      continue;
    }

    auto sub_master = dynamic_cast<EbmlMaster *>(master[idx]);
    if (sub_master)
      regenerate_uids_worker(*sub_master, new_chapter_uids);
  }
}

void
regenerate_uids(EbmlMaster &master,
                EbmlMaster *tags) {
  std::unordered_map<uint64_t, uint64_t> new_chapter_uids;

  regenerate_uids_worker(master, new_chapter_uids);

  if (tags)
    change_values<KaxTagChapterUID>(*tags, new_chapter_uids);
}

std::string
format_name_template(std::string const &name_template,
                     int chapter_number,
                     timestamp_c const &start_timestamp,
                     std::string const &appended_file_name) {
  auto name                 = name_template;
  auto number_re            = std::regex{"<NUM(?::(\\d+))?>"};
  auto timestamp_re         = std::regex{"<START(?::([^>]+))?>"};
  auto file_name_re         = std::regex{"<FILE_NAME>"};
  auto file_name_ext_re     = std::regex{"<FILE_NAME_WITH_EXT>"};
  auto appended_file_name_p = bfs::path{appended_file_name};

  name = mtx::regex::replace(name, number_re, [=](std::smatch const &match) {
    auto number_str    = fmt::format("{0}", chapter_number);
    auto wanted_length = 1u;

    if (match[1].length() && !parse_number(match[1].str(), wanted_length))
      wanted_length = 1;

    if (number_str.length() < wanted_length)
      number_str = std::string(wanted_length - number_str.length(), '0') + number_str;

    return number_str;
  });

  name = mtx::regex::replace(name, timestamp_re, [=](std::smatch const &match) {
    auto format = match[1].length() ? match[1] : "%H:%M:%S"s;
    return format_timestamp(start_timestamp.to_ns(), format);
  });

  name = std::regex_replace(name, file_name_re,     appended_file_name_p.stem().string());
  name = std::regex_replace(name, file_name_ext_re, appended_file_name_p.filename().string());

  return name;
}

void
fix_country_codes(EbmlMaster &chapters) {
  for (auto const &child : chapters) {
    auto sub_master = dynamic_cast<EbmlMaster *>(child);
    if (sub_master) {
      fix_country_codes(*sub_master);
      continue;
    }

    auto ccountry = dynamic_cast<KaxChapterCountry *>(child);
    if (!ccountry)
      continue;

    auto mapped_cctld = map_to_cctld(ccountry->GetValue());
    if (mapped_cctld)
      ccountry->SetValue(*mapped_cctld);
  }
}

}
