/*!
 * \file glyph_cache.cpp
 * \brief file glyph_cache.cpp
 *
 * Copyright 2016 by Intel.
 *
 * Contact: kevin.rogovin@intel.com
 *
 * This Source Code Form is subject to the
 * terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with
 * this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * \author Kevin Rogovin <kevin.rogovin@intel.com>
 *
 */


#include <map>
#include <vector>
#include <mutex>
#include <fastuidraw/text/glyph_cache.hpp>
#include <fastuidraw/text/glyph_render_data.hpp>
#include "../private/util_private.hpp"


namespace
{

  class GlyphCachePrivate;

  class GlyphMetricsPrivate
  {
  public:
    GlyphMetricsPrivate(GlyphCachePrivate *c, unsigned int I):
      m_cache(c),
      m_cache_location(I),
      m_ready(false),
      m_glyph_code(0),
      m_font(),
      m_horizontal_layout_offset(0.0f, 0.0f),
      m_vertical_layout_offset(0.0f, 0.0f),
      m_size(0.0f, 0.0f),
      m_advance(0.0f, 0.0f),
      m_units_per_EM(0.0f)
    {}

    GlyphMetricsPrivate():
      m_cache(nullptr),
      m_cache_location(~0u),
      m_ready(false),
      m_glyph_code(0),
      m_font(),
      m_horizontal_layout_offset(0.0f, 0.0f),
      m_vertical_layout_offset(0.0f, 0.0f),
      m_size(0.0f, 0.0f),
      m_advance(0.0f, 0.0f),
      m_units_per_EM(0.0f)
    {}

    void
    clear(void)
    {}

    /* owner */
    GlyphCachePrivate *m_cache;

    /* location into m_cache->m_glyphs  */
    unsigned int m_cache_location;

    /* if values are assigned */
    bool m_ready;

    uint32_t m_glyph_code;
    fastuidraw::reference_counted_ptr<const fastuidraw::FontBase> m_font;
    fastuidraw::vec2 m_horizontal_layout_offset;
    fastuidraw::vec2 m_vertical_layout_offset;
    fastuidraw::vec2 m_size, m_advance;
    float m_units_per_EM;
  };

  class GlyphDataPrivate
  {
  public:
    GlyphDataPrivate(GlyphCachePrivate *c, unsigned int I);
    GlyphDataPrivate(void);
    ~GlyphDataPrivate();

    void
    clear(void);

    void
    remove_from_atlas(void);

    enum fastuidraw::return_code
    upload_to_atlas(fastuidraw::GlyphLocation::Array &S);

    /* owner */
    GlyphCachePrivate *m_cache;

    /* location into m_cache->m_glyphs  */
    unsigned int m_cache_location;

    fastuidraw::GlyphRender m_render;
    GlyphMetricsPrivate *m_metrics;

    /* Location in atlas */
    std::vector<fastuidraw::GlyphLocation> m_atlas_locations;
    int m_geometry_offset, m_geometry_length;
    bool m_uploaded_to_atlas;

    /* Path of the glyph */
    fastuidraw::Path m_path;

    /* data to generate glyph data */
    fastuidraw::GlyphRenderData *m_glyph_data;
  };

  template<typename K, typename T>
  class Store
  {
  public:
    fastuidraw::c_array<T*>
    data(void)
    {
      return fastuidraw::make_c_array(m_data);
    }

    enum fastuidraw::return_code
    take(T *d, GlyphCachePrivate *c, const K &key)
    {
      if (m_map.find(key) != m_map.end())
        {
          return fastuidraw::routine_fail;
        }

      d->m_cache = c;
      d->m_cache_location = m_data.size();
      m_map[key] = m_data.size();
      m_data.push_back(d);
      return fastuidraw::routine_success;
    }

    T*
    fetch_or_allocate(GlyphCachePrivate *c, const K &key)
    {
      typename map::iterator iter;

      iter = m_map.find(key);
      if (iter != m_map.end())
        {
          return m_data[iter->second];
        }

      T *p;
      unsigned int slot;

      if (!m_free_slots.empty())
        {
          slot = m_free_slots.back();
          p = m_data[slot];
          m_free_slots.pop_back();
        }
      else
        {
          p = FASTUIDRAWnew T(c, m_data.size());
          slot = m_data.size();
          m_data.push_back(p);
        }
      m_map[key] = slot;
      return p;
    }

    void
    remove_value(const K &key)
    {
      typename map::iterator iter;

      iter = m_map.find(key);
      FASTUIDRAWassert(iter != m_map.end());

      m_data[iter->second]->clear();
      m_free_slots.push_back(iter->second);
      m_map.erase(iter);
    }

    void
    clear(void)
    {
      for (const auto &m : m_map)
        {
          m_data[m.second]->clear();
          m_free_slots.push_back(m.second);
          FASTUIDRAWassert(m.second == m_data[m.second]->m_cache_location);
        }
      m_map.clear();
    }

  private:
    typedef std::map<K, unsigned int> map;

    map m_map;
    std::vector<T*> m_data;
    std::vector<unsigned int> m_free_slots;
  };

  class GlyphSourceRender
  {
  public:
    GlyphSourceRender(void):
      m_glyph_code(0)
    {}

    GlyphSourceRender(fastuidraw::reference_counted_ptr<const fastuidraw::FontBase> f,
                      uint32_t gc, fastuidraw::GlyphRender r):
      m_font(f),
      m_glyph_code(gc),
      m_render(r)
    {
      FASTUIDRAWassert(m_render.valid());
    }

    bool
    operator<(const GlyphSourceRender &rhs) const
    {
      return (m_font != rhs.m_font) ? m_font < rhs.m_font :
        (m_glyph_code != rhs.m_glyph_code) ? m_glyph_code < rhs.m_glyph_code :
        m_render < rhs.m_render;
    }

    fastuidraw::reference_counted_ptr<const fastuidraw::FontBase> m_font;
    uint32_t m_glyph_code;
    fastuidraw::GlyphRender m_render;

    std::vector<fastuidraw::GlyphMetrics> m_temp_metrics;
  };

  class GlyphCachePrivate
  {
  public:
    explicit
    GlyphCachePrivate(fastuidraw::reference_counted_ptr<fastuidraw::GlyphAtlas> patlas,
                      fastuidraw::GlyphCache *p);

    ~GlyphCachePrivate();

    /* When the atlas is full, we will clear the atlas, but save
     *  the values in m_glyphs but mark them as not having been
     *  uploaded, this way returned values are safe and we do
     *  not have to regenerate data either.
     */

    std::mutex m_glyphs_mutex, m_glyphs_metrics_mutex;
    fastuidraw::reference_counted_ptr<fastuidraw::GlyphAtlas> m_atlas;
    Store<GlyphSourceRender, GlyphDataPrivate> m_glyphs;
    std::vector<fastuidraw::GlyphMetrics> m_tmp_metrics;
    Store<fastuidraw::GlyphSource, GlyphMetricsPrivate> m_glyph_metrics;
    fastuidraw::GlyphCache *m_p;
  };
}

/////////////////////////////////////////////////////////
// GlyphDataPrivate methods
GlyphDataPrivate::
GlyphDataPrivate(GlyphCachePrivate *c, unsigned int I):
  m_cache(c),
  m_cache_location(I),
  m_metrics(nullptr),
  m_geometry_offset(-1),
  m_geometry_length(0),
  m_uploaded_to_atlas(false),
  m_glyph_data(nullptr)
{}

GlyphDataPrivate::
GlyphDataPrivate(void):
  m_cache(nullptr),
  m_cache_location(~0u),
  m_metrics(nullptr),
  m_geometry_offset(-1),
  m_geometry_length(0),
  m_uploaded_to_atlas(false),
  m_glyph_data(nullptr)
{}

GlyphDataPrivate::
~GlyphDataPrivate()
{
  if (m_metrics && !m_metrics->m_cache)
    {
      FASTUIDRAWdelete(m_metrics);
    }
}

void
GlyphDataPrivate::
remove_from_atlas(void)
{
  if (m_cache)
    {
      for (const fastuidraw::GlyphLocation &g : m_atlas_locations)
        {
          if (g.valid())
            {
              m_cache->m_atlas->deallocate(g);
            }
        }
      m_atlas_locations.clear();

      if (m_geometry_offset != -1)
        {
          m_cache->m_atlas->deallocate_geometry_data(m_geometry_offset, m_geometry_length);
          m_geometry_offset = -1;
          m_geometry_length = 0;
        }
    }
  m_uploaded_to_atlas = false;
}

void
GlyphDataPrivate::
clear(void)
{
  m_render = fastuidraw::GlyphRender();
  FASTUIDRAWassert(!m_render.valid());

  remove_from_atlas();
  if (m_glyph_data)
    {
      FASTUIDRAWdelete(m_glyph_data);
      m_glyph_data = nullptr;
    }
  if (m_metrics && !m_metrics->m_cache)
    {
      FASTUIDRAWdelete(m_metrics);
    }
  m_metrics = nullptr;
  m_path.clear();
}

enum fastuidraw::return_code
GlyphDataPrivate::
upload_to_atlas(fastuidraw::GlyphLocation::Array &S)
{
  enum fastuidraw::return_code return_value;

  if (m_uploaded_to_atlas)
    {
      return fastuidraw::routine_success;
    }

  if (!m_cache)
    {
      return fastuidraw::routine_fail;
    }

  FASTUIDRAWassert(m_glyph_data);
  FASTUIDRAWassert(m_atlas_locations.empty());

  return_value = m_glyph_data->upload_to_atlas(m_cache->m_atlas, S,
                                               m_geometry_offset,
                                               m_geometry_length);
  if (return_value == fastuidraw::routine_success)
    {
      m_uploaded_to_atlas = true;
    }
  else
    {
      remove_from_atlas();
    }

  return return_value;
}

/////////////////////////////////////////////////
// GlyphCachePrivate methods
GlyphCachePrivate::
GlyphCachePrivate(fastuidraw::reference_counted_ptr<fastuidraw::GlyphAtlas> patlas,
                  fastuidraw::GlyphCache *p):
  m_atlas(patlas),
  m_p(p)
{}

GlyphCachePrivate::
~GlyphCachePrivate()
{
  for(GlyphDataPrivate *p : m_glyphs.data())
    {
      p->clear();
      FASTUIDRAWdelete(p);
    }

  for(GlyphMetricsPrivate *p : m_glyph_metrics.data())
    {
      FASTUIDRAWdelete(p);
    }
}

///////////////////////////////////////////////////////
// fastuidraw::Glyph methods
enum fastuidraw::glyph_type
fastuidraw::Glyph::
type(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr);
  return p->m_render.m_type;
}

fastuidraw::GlyphRender
fastuidraw::Glyph::
renderer(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr);
  return p->m_render;
}

fastuidraw::GlyphMetrics
fastuidraw::Glyph::
metrics(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return GlyphMetrics(p->m_metrics);
}

fastuidraw::reference_counted_ptr<fastuidraw::GlyphCache>
fastuidraw::Glyph::
cache(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return p->m_cache->m_p;
}

unsigned int
fastuidraw::Glyph::
cache_location(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return p->m_cache_location;
}

fastuidraw::c_array<const fastuidraw::GlyphLocation>
fastuidraw::Glyph::
atlas_locations(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return make_c_array(p->m_atlas_locations);
}

int
fastuidraw::Glyph::
geometry_offset(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return p->m_geometry_offset;
}

enum fastuidraw::return_code
fastuidraw::Glyph::
upload_to_atlas(void) const
{
  GlyphDataPrivate *p;

  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  if (!p->m_cache)
    {
      return routine_fail;
    }

  std::lock_guard<std::mutex> m(p->m_cache->m_glyphs_mutex);
  fastuidraw::GlyphLocation::Array S(&p->m_atlas_locations);
  return p->upload_to_atlas(S);
}

bool
fastuidraw::Glyph::
uploaded_to_atlas(void) const
{
  GlyphDataPrivate *p;

  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return p->m_uploaded_to_atlas;
}

const fastuidraw::Path&
fastuidraw::Glyph::
path(void) const
{
  GlyphDataPrivate *p;
  p = static_cast<GlyphDataPrivate*>(m_opaque);
  FASTUIDRAWassert(p != nullptr && p->m_render.valid());
  return p->m_path;
}

fastuidraw::Glyph
fastuidraw::Glyph::
create_glyph(GlyphRender render,
             const reference_counted_ptr<const FontBase> &font,
             uint32_t glyph_code)
{
  GlyphDataPrivate *d;
  d = FASTUIDRAWnew GlyphDataPrivate();

  d->m_metrics = FASTUIDRAWnew GlyphMetricsPrivate();
  d->m_metrics->m_font = font;
  d->m_metrics->m_glyph_code = glyph_code;

  GlyphMetricsValue v(d->m_metrics);
  GlyphMetrics cv(d->m_metrics);

  d->m_render = render;
  font->compute_metrics(glyph_code, v);
  d->m_glyph_data = font->compute_rendering_data(d->m_render, cv, d->m_path);
  return Glyph(d);
}

enum fastuidraw::return_code
fastuidraw::Glyph::
delete_glyph(Glyph G)
{
  GlyphDataPrivate *d;
  d = static_cast<GlyphDataPrivate*>(G.m_opaque);
  if (d->m_cache)
    {
      return routine_fail;
    }
  d->clear();
  FASTUIDRAWdelete(d);
  return routine_success;
}

//////////////////////////////////////////
// GlyphMetrics methods
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate, uint32_t, glyph_code)
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate,
              const fastuidraw::reference_counted_ptr<const fastuidraw::FontBase>&, font)
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate, fastuidraw::vec2, horizontal_layout_offset)
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate, fastuidraw::vec2, vertical_layout_offset)
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate, fastuidraw::vec2, size)
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate, fastuidraw::vec2, advance)
get_implement(fastuidraw::GlyphMetrics, GlyphMetricsPrivate, float, units_per_EM)

////////////////////////////////////////
// GlyphMetricsValue methods
set_implement(fastuidraw::GlyphMetricsValue, GlyphMetricsPrivate, fastuidraw::vec2, horizontal_layout_offset)
set_implement(fastuidraw::GlyphMetricsValue, GlyphMetricsPrivate, fastuidraw::vec2, vertical_layout_offset)
set_implement(fastuidraw::GlyphMetricsValue, GlyphMetricsPrivate, fastuidraw::vec2, size)
set_implement(fastuidraw::GlyphMetricsValue, GlyphMetricsPrivate, fastuidraw::vec2, advance)
set_implement(fastuidraw::GlyphMetricsValue, GlyphMetricsPrivate, float, units_per_EM)

//////////////////////////////////////////////////////////
// fastuidraw::GlyphCache methods
fastuidraw::GlyphCache::
GlyphCache(reference_counted_ptr<GlyphAtlas> patlas)
{
  m_d = FASTUIDRAWnew GlyphCachePrivate(patlas, this);
}

fastuidraw::GlyphCache::
~GlyphCache()
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);
  FASTUIDRAWdelete(d);
  m_d = nullptr;
}

fastuidraw::GlyphMetrics
fastuidraw::GlyphCache::
fetch_glyph_metrics(const fastuidraw::reference_counted_ptr<const FontBase> &font,
                    uint32_t glyph_code)
{
  if (!font)
    {
      return GlyphMetrics();
    }

  GlyphCachePrivate *d;
  GlyphMetricsPrivate *p;

  d = static_cast<GlyphCachePrivate*>(m_d);
  std::lock_guard<std::mutex> m(d->m_glyphs_metrics_mutex);
  p = d->m_glyph_metrics.fetch_or_allocate(d, GlyphSource(glyph_code, font));
  if (!p->m_ready)
    {
      GlyphMetricsValue v(p);
      p->m_font = font;
      p->m_glyph_code = glyph_code;
      font->compute_metrics(glyph_code, v);
      p->m_ready = true;
    }
  return GlyphMetrics(p);
}

void
fastuidraw::GlyphCache::
fetch_glyph_metrics(const reference_counted_ptr<const FontBase> &font,
                    c_array<const uint32_t> glyph_codes,
                    c_array<GlyphMetrics> out_metrics)
{

  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  if (!font)
    {
      return std::fill(out_metrics.begin(), out_metrics.end(), GlyphMetrics());
    }

  std::lock_guard<std::mutex> m(d->m_glyphs_metrics_mutex);

  for (unsigned int i = 0; i < glyph_codes.size(); ++i)
    {
      GlyphMetricsPrivate *p;

      p = d->m_glyph_metrics.fetch_or_allocate(d, GlyphSource(glyph_codes[i], font));
      if (!p->m_ready)
        {
          GlyphMetricsValue v(p);
          p->m_font = font;
          p->m_glyph_code = glyph_codes[i];
          font->compute_metrics(glyph_codes[i], v);
          p->m_ready = true;
        }
      out_metrics[i] = GlyphMetrics(p);
    }
}

void
fastuidraw::GlyphCache::
fetch_glyph_metrics(c_array<const GlyphSource> glyph_sources,
                    c_array<GlyphMetrics> out_metrics)
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);
  std::lock_guard<std::mutex> m(d->m_glyphs_metrics_mutex);

  for (unsigned int i = 0; i < glyph_sources.size(); ++i)
    {
      GlyphMetricsPrivate *p;

      if (glyph_sources[i].m_font)
        {
          p = d->m_glyph_metrics.fetch_or_allocate(d, glyph_sources[i]);
          if (!p->m_ready)
            {
              GlyphMetricsValue v(p);
              p->m_font = glyph_sources[i].m_font;
              p->m_glyph_code = glyph_sources[i].m_glyph_code;
              glyph_sources[i].m_font->compute_metrics(glyph_sources[i].m_glyph_code, v);
              p->m_ready = true;
            }
        }
      else
        {
          p = nullptr;
        }
      out_metrics[i] = GlyphMetrics(p);
    }
}

fastuidraw::Glyph
fastuidraw::GlyphCache::
fetch_glyph(GlyphRender render,
            const fastuidraw::reference_counted_ptr<const FontBase> &font,
            uint32_t glyph_code, bool upload_to_atlas)
{
  if (!font || !font->can_create_rendering_data(render.m_type))
    {
      return Glyph();
    }

  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  GlyphDataPrivate *q;
  GlyphSourceRender src(font, glyph_code, render);

  std::lock_guard<std::mutex> m(d->m_glyphs_mutex);
  q = d->m_glyphs.fetch_or_allocate(d, src);

  if (!q->m_render.valid())
    {
      GlyphMetrics m;

      q->m_render = render;
      FASTUIDRAWassert(!q->m_glyph_data);
      m = fetch_glyph_metrics(font, glyph_code);
      q->m_metrics = static_cast<GlyphMetricsPrivate*>(m.m_d);
      q->m_glyph_data = font->compute_rendering_data(q->m_render, m, q->m_path);
    }

  if (upload_to_atlas)
    {
      fastuidraw::GlyphLocation::Array S(&q->m_atlas_locations);
      q->upload_to_atlas(S);
    }

  return Glyph(q);
}

void
fastuidraw::GlyphCache::
fetch_glyphs(GlyphRender render,
             c_array<const GlyphSource> glyph_sources,
             c_array<Glyph> out_glyphs,
             bool upload_to_atlas)
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  std::lock_guard<std::mutex> m(d->m_glyphs_mutex);
  d->m_tmp_metrics.resize(glyph_sources.size());
  fetch_glyph_metrics(glyph_sources, make_c_array(d->m_tmp_metrics));

  for(unsigned int i = 0; i < glyph_sources.size(); ++i)
    {
      if (glyph_sources[i].m_font)
        {
          GlyphSourceRender src(glyph_sources[i].m_font, glyph_sources[i].m_glyph_code, render);
          GlyphDataPrivate *q;

          q = d->m_glyphs.fetch_or_allocate(d, src);
          if (!q->m_render.valid())
            {
              GlyphMetrics m(d->m_tmp_metrics[i]);

              q->m_render = render;
              FASTUIDRAWassert(!q->m_glyph_data);
              q->m_metrics = static_cast<GlyphMetricsPrivate*>(m.m_d);
              q->m_glyph_data = glyph_sources[i].m_font->compute_rendering_data(q->m_render, m, q->m_path);
            }

          if (upload_to_atlas)
            {
              fastuidraw::GlyphLocation::Array S(&q->m_atlas_locations);
              q->upload_to_atlas(S);
            }

          out_glyphs[i] = Glyph(q);
        }
      else
        {
          out_glyphs[i] = Glyph();
        }
    }
}

void
fastuidraw::GlyphCache::
fetch_glyphs(GlyphRender render,
             const reference_counted_ptr<const FontBase> &font,
             c_array<const uint32_t> glyph_codes,
             c_array<Glyph> out_glyphs,
             bool upload_to_atlas)
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  std::lock_guard<std::mutex> m(d->m_glyphs_mutex);
  d->m_tmp_metrics.resize(glyph_codes.size());
  fetch_glyph_metrics(font, glyph_codes, make_c_array(d->m_tmp_metrics));

  for(unsigned int i = 0; i < glyph_codes.size(); ++i)
    {
      GlyphSourceRender src(font, glyph_codes[i], render);
      GlyphDataPrivate *q;

      q = d->m_glyphs.fetch_or_allocate(d, src);
      if (!q->m_render.valid())
        {
          GlyphMetrics m(d->m_tmp_metrics[i]);

          q->m_render = render;
          FASTUIDRAWassert(!q->m_glyph_data);
          q->m_metrics = static_cast<GlyphMetricsPrivate*>(m.m_d);
          q->m_glyph_data = font->compute_rendering_data(q->m_render, m, q->m_path);
        }

      if (upload_to_atlas)
        {
          fastuidraw::GlyphLocation::Array S(&q->m_atlas_locations);
          q->upload_to_atlas(S);
        }

      out_glyphs[i] = Glyph(q);
    }
}

enum fastuidraw::return_code
fastuidraw::GlyphCache::
add_glyph(Glyph glyph, bool upload_to_atlas)
{
  GlyphDataPrivate *g;
  g = static_cast<GlyphDataPrivate*>(glyph.m_opaque);

  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  if (!g)
    {
      return routine_fail;
    }

  if (g->m_cache && g->m_cache != d)
    {
      return routine_fail;
    }

  std::lock_guard<std::mutex> m(d->m_glyphs_mutex);
  if (g->m_cache)
    {
      if (upload_to_atlas)
        {
          fastuidraw::GlyphLocation::Array S(&g->m_atlas_locations);
          g->upload_to_atlas(S);
        }
      return routine_success;
    }

  GlyphSourceRender src(g->m_metrics->m_font,
                        g->m_metrics->m_glyph_code,
                        g->m_render);

  if (d->m_glyphs.take(g, d, src) == routine_fail)
    {
      return routine_fail;
    }

  if (upload_to_atlas)
    {
      fastuidraw::GlyphLocation::Array S(&g->m_atlas_locations);
      g->upload_to_atlas(S);
    }

  return routine_success;
}

void
fastuidraw::GlyphCache::
delete_glyph(Glyph G)
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  GlyphDataPrivate *g;
  g = static_cast<GlyphDataPrivate*>(G.m_opaque);
  FASTUIDRAWassert(g != nullptr);
  FASTUIDRAWassert(g->m_cache == d);
  FASTUIDRAWassert(g->m_render.valid());

  GlyphSourceRender src(g->m_metrics->m_font,
                        g->m_metrics->m_glyph_code,
                        g->m_render);
  std::lock_guard<std::mutex> m(d->m_glyphs_mutex);
  d->m_glyphs.remove_value(src);
}

void
fastuidraw::GlyphCache::
clear_atlas(void)
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  std::lock_guard<std::mutex> m(d->m_glyphs_mutex);
  d->m_atlas->clear();
  for(GlyphDataPrivate *g : d->m_glyphs.data())
    {
      g->m_uploaded_to_atlas = false;
      g->m_atlas_locations.clear();
      g->m_geometry_offset = -1;
      g->m_geometry_length = 0;
    }
}

void
fastuidraw::GlyphCache::
clear_cache(void)
{
  GlyphCachePrivate *d;
  d = static_cast<GlyphCachePrivate*>(m_d);

  std::lock_guard<std::mutex> m1(d->m_glyphs_mutex);
  std::lock_guard<std::mutex> m2(d->m_glyphs_metrics_mutex);
  d->m_atlas->clear();
  d->m_glyphs.clear();
  d->m_glyph_metrics.clear();
}
