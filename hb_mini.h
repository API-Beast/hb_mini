#ifndef HB_MINI_H
#define HB_MINI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int hb_bool_t;
typedef uint32_t hb_codepoint_t;
typedef int32_t hb_position_t;
typedef uint32_t hb_mask_t;
typedef uint32_t hb_tag_t;
typedef uint32_t hb_color_t;

#define HB_TAG(c1,c2,c3,c4) ((hb_tag_t)((((uint32_t)(c1)&0xFF)<<24)|(((uint32_t)(c2)&0xFF)<<16)|(((uint32_t)(c3)&0xFF)<<8)|((uint32_t)(c4)&0xFF)))
#define HB_UNTAG(tag) (uint8_t)(((tag)>>24)&0xFF), (uint8_t)(((tag)>>16)&0xFF), (uint8_t)(((tag)>>8)&0xFF), (uint8_t)((tag)&0xFF)

typedef struct hb_blob_t hb_blob_t;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;
typedef struct hb_raster_paint_t hb_raster_paint_t;
typedef struct hb_raster_image_t hb_raster_image_t;

typedef union hb_var_int_t {
  uint32_t u32;
  int32_t i32;
  uint16_t u16[2];
  int16_t i16[2];
  uint8_t u8[4];
  int8_t i8[4];
} hb_var_int_t;

typedef struct hb_glyph_info_t {
  hb_codepoint_t codepoint;
  hb_mask_t mask;
  uint32_t cluster;
  hb_var_int_t var1;
  hb_var_int_t var2;
} hb_glyph_info_t;

typedef struct hb_glyph_position_t {
  hb_position_t x_advance;
  hb_position_t y_advance;
  hb_position_t x_offset;
  hb_position_t y_offset;
  hb_var_int_t var;
} hb_glyph_position_t;

typedef struct hb_glyph_extents_t {
  hb_position_t x_bearing;
  hb_position_t y_bearing;
  hb_position_t width;
  hb_position_t height;
} hb_glyph_extents_t;

typedef struct hb_font_extents_t {
  hb_position_t ascender;
  hb_position_t descender;
  hb_position_t line_gap;
  hb_position_t reserved[9];
} hb_font_extents_t;

typedef struct hb_variation_t {
  hb_tag_t tag;
  float value;
} hb_variation_t;

typedef struct hb_ot_var_axis_info_t {
  unsigned int axis_index;
  hb_tag_t tag;
  unsigned int name_id;
  unsigned int flags;
  float min_value;
  float default_value;
  float max_value;
  unsigned int reserved;
} hb_ot_var_axis_info_t;

typedef struct hb_raster_extents_t {
  int x_origin, y_origin;
  unsigned int width, height;
  unsigned int stride;
} hb_raster_extents_t;

typedef enum { HB_STYLE_TAG_WEIGHT = HB_TAG('w','g','h','t') } hb_style_tag_t;

hb_blob_t *hb_blob_create_from_file_or_fail (const char *file_name);
void hb_blob_destroy (hb_blob_t *blob);

hb_face_t *hb_face_create (hb_blob_t *blob, unsigned int index);
void hb_face_destroy (hb_face_t *face);
unsigned int hb_face_get_glyph_count (const hb_face_t *face);

hb_font_t *hb_font_create (hb_face_t *face);
void hb_font_destroy (hb_font_t *font);
void hb_font_set_scale (hb_font_t *font, int x_scale, int y_scale);
void hb_font_set_ppem (hb_font_t *font, unsigned int x_ppem, unsigned int y_ppem);
void hb_font_set_variations (hb_font_t *font, const hb_variation_t *variations, unsigned int variations_length);
void hb_font_set_synthetic_bold (hb_font_t *font, float x_embolden, float y_embolden, hb_bool_t in_place);
void hb_font_get_synthetic_bold (hb_font_t *font, float *x_embolden, float *y_embolden, hb_bool_t *in_place);
void hb_font_set_synthetic_slant (hb_font_t *font, float slant);
hb_bool_t hb_font_get_h_extents (hb_font_t *font, hb_font_extents_t *extents);
hb_bool_t hb_font_get_nominal_glyph (hb_font_t *font, hb_codepoint_t unicode, hb_codepoint_t *glyph);
hb_position_t hb_font_get_glyph_h_advance (hb_font_t *font, hb_codepoint_t glyph);
hb_bool_t hb_font_get_glyph_extents (hb_font_t *font, hb_codepoint_t glyph, hb_glyph_extents_t *extents);
float hb_style_get_value (hb_font_t *font, hb_style_tag_t style_tag);

unsigned int hb_ot_var_get_axis_count (hb_face_t *face);
unsigned int hb_ot_var_get_axis_infos (hb_face_t *face, unsigned int start_offset, unsigned int *axes_count, hb_ot_var_axis_info_t *axes_array);
typedef struct hb_ot_color_layer_t { hb_codepoint_t glyph; unsigned int color_index; } hb_ot_color_layer_t;
hb_bool_t hb_ot_color_glyph_has_paint (hb_face_t *face, hb_codepoint_t glyph);
unsigned int hb_ot_color_glyph_get_layers (hb_face_t *face, hb_codepoint_t glyph, unsigned int start_offset, unsigned int *layer_count, hb_ot_color_layer_t *layers);
hb_bool_t hb_ot_color_has_png (hb_face_t *face);

hb_buffer_t *hb_buffer_create (void);
void hb_buffer_destroy (hb_buffer_t *buffer);
void hb_buffer_clear_contents (hb_buffer_t *buffer);
void hb_buffer_add_utf8 (hb_buffer_t *buffer, const char *text, int text_length, unsigned int item_offset, int item_length);
void hb_buffer_guess_segment_properties (hb_buffer_t *buffer);
hb_glyph_info_t *hb_buffer_get_glyph_infos (hb_buffer_t *buffer, unsigned int *length);
hb_glyph_position_t *hb_buffer_get_glyph_positions (hb_buffer_t *buffer, unsigned int *length);
void hb_shape (hb_font_t *font, hb_buffer_t *buffer, const void *features, unsigned int num_features);

hb_raster_paint_t *hb_raster_paint_create_or_fail (void);
void hb_raster_paint_destroy (hb_raster_paint_t *paint);
void hb_raster_paint_reset (hb_raster_paint_t *paint);
void hb_raster_paint_set_transform (hb_raster_paint_t *paint, float xx, float yx, float xy, float yy, float dx, float dy);
void hb_raster_paint_set_scale_factor (hb_raster_paint_t *paint, float x_scale_factor, float y_scale_factor);
hb_bool_t hb_raster_paint_set_glyph_extents (hb_raster_paint_t *paint, const hb_glyph_extents_t *glyph_extents);
void hb_raster_paint_set_foreground (hb_raster_paint_t *paint, hb_color_t foreground);
void hb_raster_paint_glyph (hb_raster_paint_t *paint, hb_font_t *font, hb_codepoint_t glyph);
hb_raster_image_t *hb_raster_paint_render (hb_raster_paint_t *paint);
void hb_raster_paint_recycle_image (hb_raster_paint_t *paint, hb_raster_image_t *image);
const uint8_t *hb_raster_image_get_buffer (const hb_raster_image_t *image);
void hb_raster_image_get_extents (const hb_raster_image_t *image, hb_raster_extents_t *extents);

#ifdef __cplusplus
}
#endif

#endif
