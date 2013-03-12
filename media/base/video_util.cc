// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_util.h"

#include <cmath>

#include "base/logging.h"
#include "media/base/video_frame.h"

namespace media {

gfx::Size GetNaturalSize(const gfx::Size& visible_size,
                         int aspect_ratio_numerator,
                         int aspect_ratio_denominator) {
  if (aspect_ratio_denominator == 0 ||
      aspect_ratio_numerator < 0 ||
      aspect_ratio_denominator < 0)
    return gfx::Size();

  double aspect_ratio = aspect_ratio_numerator /
      static_cast<double>(aspect_ratio_denominator);

  int width = floor(visible_size.width() * aspect_ratio + 0.5);
  int height = visible_size.height();

  // An even width makes things easier for YV12 and appears to be the behavior
  // expected by WebKit layout tests.
  return gfx::Size(width & ~1, height);
}

void CopyPlane(size_t plane, const uint8* source, int stride, int rows,
               VideoFrame* frame) {
  uint8* dest = frame->data(plane);
  int dest_stride = frame->stride(plane);

  // Clamp in case source frame has smaller stride.
  int bytes_to_copy_per_row = std::min(frame->row_bytes(plane), stride);

  // Clamp in case source frame has smaller height.
  int rows_to_copy = std::min(frame->rows(plane), rows);

  // Copy!
  for (int row = 0; row < rows_to_copy; ++row) {
    memcpy(dest, source, bytes_to_copy_per_row);
    source += stride;
    dest += dest_stride;
  }
}

void CopyYPlane(const uint8* source, int stride, int rows, VideoFrame* frame) {
  CopyPlane(VideoFrame::kYPlane, source, stride, rows, frame);
}

void CopyUPlane(const uint8* source, int stride, int rows, VideoFrame* frame) {
  CopyPlane(VideoFrame::kUPlane, source, stride, rows, frame);
}

void CopyVPlane(const uint8* source, int stride, int rows, VideoFrame* frame) {
  CopyPlane(VideoFrame::kVPlane, source, stride, rows, frame);
}

void FillYUV(VideoFrame* frame, uint8 y, uint8 u, uint8 v) {
  // Fill the Y plane.
  uint8* y_plane = frame->data(VideoFrame::kYPlane);
  int y_rows = frame->rows(VideoFrame::kYPlane);
  int y_row_bytes = frame->row_bytes(VideoFrame::kYPlane);
  for (int i = 0; i < y_rows; ++i) {
    memset(y_plane, y, y_row_bytes);
    y_plane += frame->stride(VideoFrame::kYPlane);
  }

  // Fill the U and V planes.
  uint8* u_plane = frame->data(VideoFrame::kUPlane);
  uint8* v_plane = frame->data(VideoFrame::kVPlane);
  int uv_rows = frame->rows(VideoFrame::kUPlane);
  int u_row_bytes = frame->row_bytes(VideoFrame::kUPlane);
  int v_row_bytes = frame->row_bytes(VideoFrame::kVPlane);
  for (int i = 0; i < uv_rows; ++i) {
    memset(u_plane, u, u_row_bytes);
    memset(v_plane, v, v_row_bytes);
    u_plane += frame->stride(VideoFrame::kUPlane);
    v_plane += frame->stride(VideoFrame::kVPlane);
  }
}

void RotatePlaneByPixels(
    uint8* src,
    uint8* dest,
    int width,
    int height,
    int rotation,  // Clockwise.
    bool flip_vert,
    bool flip_horiz) {
  DCHECK((width > 0) && (height > 0) &&
         ((width & 1) == 0) && ((height & 1) == 0) &&
         (rotation >= 0) && (rotation < 360) && (rotation % 90 == 0));

  // Consolidate cases. Only 0 and 90 are left.
  if (rotation == 180 || rotation == 270) {
    rotation -= 180;
    flip_vert = !flip_vert;
    flip_horiz = !flip_horiz;
  }

  int num_rows = height;
  int num_cols = width;
  int src_stride = width;
  // During pixel copying, the corresponding incremental of dest pointer
  // when src pointer moves to next row.
  int dest_row_step = width;
  // During pixel copying, the corresponding incremental of dest pointer
  // when src pointer moves to next column.
  int dest_col_step = 1;

  if (rotation == 0) {
    if (flip_horiz) {
      // Use pixel copying.
      dest_col_step = -1;
      if (flip_vert) {
        // Rotation 180.
        dest_row_step = -width;
        dest += height * width - 1;
      } else {
        dest += width - 1;
      }
    } else {
      if (flip_vert) {
        // Fast copy by rows.
        dest += width * (height - 1);
        for (int row = 0; row < height; ++row) {
          memcpy(dest, src, width);
          src += width;
          dest -= width;
        }
      } else {
        memcpy(dest, src, width * height);
      }
      return;
    }
  } else if (rotation == 90) {
    int offset;
    if (width > height) {
      offset = (width - height) / 2;
      src += offset;
      num_rows = num_cols = height;
    } else {
      offset = (height - width) / 2;
      src += width * offset;
      num_rows = num_cols = width;
    }

    dest_col_step = (flip_vert ? -width : width);
    dest_row_step = (flip_horiz ? 1 : -1);
    if (flip_horiz) {
      if (flip_vert) {
        dest += (width > height ? width * (height - 1) + offset :
                                  width * (height - offset - 1));
      } else {
        dest += (width > height ? offset : width * offset);
      }
    } else {
      if (flip_vert) {
        dest += (width > height ?  width * height - offset - 1 :
                                   width * (height - offset) - 1);
      } else {
        dest += (width > height ? width - offset - 1 :
                                  width * (offset + 1) - 1);
      }
    }
  } else {
    NOTREACHED();
  }

  // Copy pixels.
  for (int row = 0; row < num_rows; ++row) {
    uint8* src_ptr = src;
    uint8* dest_ptr = dest;
    for (int col = 0; col < num_cols; ++col) {
      *dest_ptr = *src_ptr++;
      dest_ptr += dest_col_step;
    }
    src += src_stride;
    dest += dest_row_step;
  }
}

}  // namespace media
