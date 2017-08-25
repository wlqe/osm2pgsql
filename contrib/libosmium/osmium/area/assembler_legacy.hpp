#ifndef OSMIUM_AREA_ASSEMBLER_LEGACY_HPP
#define OSMIUM_AREA_ASSEMBLER_LEGACY_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013-2017 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <map>
#include <utility>
#include <vector>

#include <osmium/area/assembler_config.hpp>
#include <osmium/area/detail/basic_assembler_with_tags.hpp>
#include <osmium/area/detail/proto_ring.hpp>
#include <osmium/area/detail/segment_list.hpp>
#include <osmium/area/problem_reporter.hpp>
#include <osmium/area/stats.hpp>
#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/memory/collection.hpp>
#include <osmium/osm/area.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/node_ref.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/tag.hpp>
#include <osmium/osm/way.hpp>
#include <osmium/tags/filter.hpp>

namespace osmium {

    namespace area {

        /**
         * Assembles area objects from closed ways or multipolygon relations
         * and their members.
         */
        class AssemblerLegacy : public detail::BasicAssemblerWithTags {

            void add_tags_to_area(osmium::builder::AreaBuilder& builder, const osmium::Way& way) const {
                builder.add_item(way.tags());
            }

            void add_common_tags(osmium::builder::TagListBuilder& tl_builder, std::set<const osmium::Way*>& ways) const {
                std::map<std::string, std::size_t> counter;
                for (const osmium::Way* way : ways) {
                    for (const auto& tag : way->tags()) {
                        std::string kv{tag.key()};
                        kv.append(1, '\0');
                        kv.append(tag.value());
                        ++counter[kv];
                    }
                }

                const std::size_t num_ways = ways.size();
                for (const auto& t_c : counter) {
                    if (debug()) {
                        std::cerr << "        tag " << t_c.first << " is used " << t_c.second << " times in " << num_ways << " ways\n";
                    }
                    if (t_c.second == num_ways) {
                        const std::size_t len = std::strlen(t_c.first.c_str());
                        tl_builder.add_tag(t_c.first.c_str(), t_c.first.c_str() + len + 1);
                    }
                }
            }

            struct MPFilter : public osmium::tags::KeyFilter {

                MPFilter() : osmium::tags::KeyFilter(true) {
                    add(false, "type");
                    add(false, "created_by");
                    add(false, "source");
                    add(false, "note");
                    add(false, "test:id");
                    add(false, "test:section");
                }

            }; // struct MPFilter

            static const MPFilter& filter() noexcept {
                static const MPFilter filter;
                return filter;
            }

            void add_tags_to_area(osmium::builder::AreaBuilder& builder, const osmium::Relation& relation) {
                const auto count = std::count_if(relation.tags().cbegin(), relation.tags().cend(), std::cref(filter()));

                if (debug()) {
                    std::cerr << "  found " << count << " tags on relation (without ignored ones)\n";
                }

                if (count > 0) {
                    if (debug()) {
                        std::cerr << "    use tags from relation\n";
                    }

                    if (config().keep_type_tag) {
                        builder.add_item(relation.tags());
                    } else {
                        copy_tags_without_type(builder, relation.tags());
                    }
                } else {
                    ++stats().no_tags_on_relation;
                    if (debug()) {
                        std::cerr << "    use tags from outer ways\n";
                    }
                    std::set<const osmium::Way*> ways;
                    for (const auto& ring : rings()) {
                        if (ring.is_outer()) {
                            ring.get_ways(ways);
                        }
                    }
                    if (ways.size() == 1) {
                        if (debug()) {
                            std::cerr << "      only one outer way\n";
                        }
                        builder.add_item((*ways.cbegin())->tags());
                    } else {
                        if (debug()) {
                            std::cerr << "      multiple outer ways, get common tags\n";
                        }
                        osmium::builder::TagListBuilder tl_builder{builder};
                        add_common_tags(tl_builder, ways);
                    }
                }
            }

            bool create_area(osmium::memory::Buffer& out_buffer, const osmium::Way& way) {
                osmium::builder::AreaBuilder builder{out_buffer};
                builder.initialize_from_object(way);

                const bool area_okay = create_rings();
                if (area_okay || config().create_empty_areas) {
                    add_tags_to_area(builder, way);
                }
                if (area_okay) {
                    add_rings_to_area(builder);
                }

                if (report_ways()) {
                    config().problem_reporter->report_way(way);
                }

                return area_okay || config().create_empty_areas;
            }

            bool create_area(osmium::memory::Buffer& out_buffer, const osmium::Relation& relation, const std::vector<const osmium::Way*>& members) {
                set_num_members(members.size());
                osmium::builder::AreaBuilder builder{out_buffer};
                builder.initialize_from_object(relation);

                const bool area_okay = create_rings();
                if (area_okay || config().create_empty_areas) {
                    add_tags_to_area(builder, relation);
                }
                if (area_okay) {
                    add_rings_to_area(builder);
                }

                if (report_ways()) {
                    for (const osmium::Way* way : members) {
                        config().problem_reporter->report_way(*way);
                    }
                }

                return area_okay || config().create_empty_areas;
            }

        public:

            explicit AssemblerLegacy(const config_type& config) :
                detail::BasicAssemblerWithTags(config) {
            }

            ~AssemblerLegacy() noexcept = default;

            /**
             * Assemble an area from the given way.
             * The resulting area is put into the out_buffer.
             *
             * @returns false if there was some kind of error building the
             *          area, true otherwise.
             */
            bool operator()(const osmium::Way& way, osmium::memory::Buffer& out_buffer) {
                if (!config().create_way_polygons) {
                    return true;
                }

                if (way.tags().has_tag("area", "no")) {
                    return true;
                }

                if (config().problem_reporter) {
                    config().problem_reporter->set_object(osmium::item_type::way, way.id());
                    config().problem_reporter->set_nodes(way.nodes().size());
                }

                // Ignore (but count) ways without segments.
                if (way.nodes().size() < 2) {
                    ++stats().short_ways;
                    return false;
                }

                if (!way.ends_have_same_id()) {
                    ++stats().duplicate_nodes;
                    if (config().problem_reporter) {
                        config().problem_reporter->report_duplicate_node(way.nodes().front().ref(), way.nodes().back().ref(), way.nodes().front().location());
                    }
                }

                ++stats().from_ways;
                stats().invalid_locations = segment_list().extract_segments_from_way(config().problem_reporter,
                                                                                     stats().duplicate_nodes,
                                                                                     way);
                if (!config().ignore_invalid_locations && stats().invalid_locations > 0) {
                    return false;
                }

                if (config().debug_level > 0) {
                    std::cerr << "\nAssembling way " << way.id() << " containing " << segment_list().size() << " nodes\n";
                }

                // Now create the Area object and add the attributes and tags
                // from the way.
                const bool okay = create_area(out_buffer, way);
                if (okay) {
                    out_buffer.commit();
                } else {
                    out_buffer.rollback();
                }

                if (debug()) {
                    std::cerr << "Done: " << stats() << "\n";
                }

                return okay;
            }

            /**
             * Assemble an area from the given relation and its members.
             * The resulting area is put into the out_buffer.
             *
             * @returns false if there was some kind of error building the
             *          area(s), true otherwise.
             */
            bool operator()(const osmium::Relation& relation, const std::vector<const osmium::Way*>& members, osmium::memory::Buffer& out_buffer) {
                assert(relation.members().size() >= members.size());

                if (config().problem_reporter) {
                    config().problem_reporter->set_object(osmium::item_type::relation, relation.id());
                }

                if (relation.members().empty()) {
                    ++stats().no_way_in_mp_relation;
                    return false;
                }

                ++stats().from_relations;
                stats().invalid_locations = segment_list().extract_segments_from_ways(config().problem_reporter,
                                                                                      stats().duplicate_nodes,
                                                                                      stats().duplicate_ways,
                                                                                      relation,
                                                                                      members);
                if (!config().ignore_invalid_locations && stats().invalid_locations > 0) {
                    return false;
                }
                stats().member_ways = members.size();

                if (stats().member_ways == 1) {
                    ++stats().single_way_in_mp_relation;
                }

                if (config().debug_level > 0) {
                    std::cerr << "\nAssembling relation " << relation.id() << " containing " << members.size() << " way members with " << segment_list().size() << " nodes\n";
                }

                const std::size_t area_offset = out_buffer.committed();

                // Now create the Area object and add the attributes and tags
                // from the relation.
                bool okay = create_area(out_buffer, relation, members);
                if (okay) {
                    if ((config().create_new_style_polygons && stats().no_tags_on_relation == 0) ||
                        (config().create_old_style_polygons && stats().no_tags_on_relation != 0)) {
                        out_buffer.commit();
                    } else {
                        out_buffer.rollback();
                    }
                } else {
                    out_buffer.rollback();
                }

                const osmium::TagList& area_tags = out_buffer.get<osmium::Area>(area_offset).tags(); // tags of the area we just built

                // Find all closed ways that are inner rings and check their
                // tags. If they are not the same as the tags of the area we
                // just built, add them to a list and later build areas for
                // them, too.
                std::vector<const osmium::Way*> ways_that_should_be_areas;
                if (stats().wrong_role == 0) {
                    detail::for_each_member(relation, members, [this, &ways_that_should_be_areas, &area_tags](const osmium::RelationMember& member, const osmium::Way& way) {
                        if (!std::strcmp(member.role(), "inner")) {
                            if (!way.nodes().empty() && way.is_closed() && way.tags().size() > 0) {
                                const auto d = std::count_if(way.tags().cbegin(), way.tags().cend(), std::cref(filter()));
                                if (d > 0) {
                                    osmium::tags::KeyFilter::iterator way_fi_begin(std::cref(filter()), way.tags().cbegin(), way.tags().cend());
                                    osmium::tags::KeyFilter::iterator way_fi_end(std::cref(filter()), way.tags().cend(), way.tags().cend());
                                    osmium::tags::KeyFilter::iterator area_fi_begin(std::cref(filter()), area_tags.cbegin(), area_tags.cend());
                                    osmium::tags::KeyFilter::iterator area_fi_end(std::cref(filter()), area_tags.cend(), area_tags.cend());

                                    if (!std::equal(way_fi_begin, way_fi_end, area_fi_begin) || d != std::distance(area_fi_begin, area_fi_end)) {
                                        ways_that_should_be_areas.push_back(&way);
                                    } else {
                                        ++stats().inner_with_same_tags;
                                        if (config().problem_reporter) {
                                            config().problem_reporter->report_inner_with_same_tags(way);
                                        }
                                    }
                                }
                            }
                        }
                    });
                }

                if (debug()) {
                    std::cerr << "Done: " << stats() << "\n";
                }

                // Now build areas for all ways found in the last step.
                for (const osmium::Way* way : ways_that_should_be_areas) {
                    AssemblerLegacy assembler{config()};
                    if (!assembler(*way, out_buffer)) {
                        okay = false;
                    }
                    stats() += assembler.stats();
                }

                return okay;
            }

        }; // class AssemblerLegacy

    } // namespace area

} // namespace osmium

#endif // OSMIUM_AREA_ASSEMBLER_LEGACY_HPP
