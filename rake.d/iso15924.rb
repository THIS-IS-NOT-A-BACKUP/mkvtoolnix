def create_iso15924_script_list_file
  content = Mtx::OnlineFile.download("https://unicode.org/iso15924/iso15924.txt")

  entries = content.
    force_encoding("UTF-8").
    split(%r{\n+}).
    map(&:chomp).
    reject { |line| %r{^#}.match(line) }.
    reject { |line| %r{^Qa(a|b[a-x])}.match(line) }.
    select { |line| %r{;.*;.*;}.match(line) }.
    map    { |line| line.split(';') }.
    map    { |line| [
      line[0][0..0].upcase + line[0][1..line[0].length].downcase,
      line[1].gsub(%r{^0}, '').to_i,
      line[2],
    ] }

  (0..49).map do |idx|
    entries << [
      sprintf('Qa%s%s', ('a'.ord + (idx / 26)).chr, ('a'.ord + (idx % 26)).chr),
      900 + idx,
      'Reserved for private use',
    ]
  end

  entry_map = Hash[ *
    entries.map { |e| [ e[0], e ] }.flatten(1) +
    entries.map { |e| [ e[1], e ] }.flatten(1)
  ]

  Mtx::IANALanguageSubtagRegistry.
    fetch_registry["script"].
    reject { |entry| %r{\.\.}.match(entry[:subtag]) }.
    each do |entry|

    if %r{^[0-9]+$}.match(entry[:subtag])
      number = entry[:subtag].gsub(%r{^0+}, '').to_i
      code   = ""
      idx    = number
    else
      number = 0
      code   = entry[:subtag]
      idx    = code
    end

    if !entry_map.key?(idx)
      entry_map[key] = [
        code,
        number,
        entry[:description],
      ]
    end

    entry_map[idx] << entry.key?(:deprecated)
  end

  rows = entries.
    map { |entry| [
      entry[0].to_c_string,
      sprintf('%03s', entry[1]),
      entry[2].to_u8_c_string,
      (entry[3] || false).to_s,
    ] }


  header = <<EOT
/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL v2
   see the file COPYING for details
   or visit https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

   ISO 15924 script list

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

// -------------------------------------------------------------------------
// NOTE: this file is auto-generated by the "dev:iso15924_list" rake target.
// -------------------------------------------------------------------------

#include "common/common_pch.h"

#include "common/iso15924.h"

namespace mtx::iso15924 {

std::vector<script_t> g_scripts;

struct script_init_t {
  char const *code;
  unsigned int number;
  char const *english_name;
  bool is_deprecated;
};

static script_init_t const s_scripts_init[] = {
EOT

  footer = <<EOT
};

void
init() {
  g_scripts.reserve(#{rows.size});

  for (script_init_t const *script = s_scripts_init, *end = script + #{rows.size}; script < end; ++script)
    g_scripts.emplace_back(script->code, script->number, script->english_name, script->is_deprecated);
}

} // namespace mtx::iso15924
EOT

  content       = header + format_table(rows.sort, :column_suffix => ',', :row_prefix => "  { ", :row_suffix => " },").join("\n") + "\n" + footer
  cpp_file_name = "src/common/iso15924_script_list.cpp"

  runq("write", cpp_file_name) { IO.write("#{$source_dir}/#{cpp_file_name}", content); 0 }
end
