#include "doccore/layout.h"

namespace doccore {

DrawPath MakeRect(double x, double y, double w, double h, Rgba fill, bool stroke, Rgba stroke_color,
                  double stroke_pt) {
  DrawPath p;
  p.has_fill = true;
  p.has_stroke = stroke;
  p.fill = fill;
  p.stroke = stroke_color;
  p.stroke_pt = stroke_pt;
  PathVerb v;
  v.kind = PathVerbKind::MoveTo;
  v.x1 = x;
  v.y1 = y;
  p.verbs.push_back(v);
  v.kind = PathVerbKind::LineTo;
  v.x1 = x + w;
  v.y1 = y;
  p.verbs.push_back(v);
  v.x1 = x + w;
  v.y1 = y + h;
  p.verbs.push_back(v);
  v.x1 = x;
  v.y1 = y + h;
  p.verbs.push_back(v);
  v.kind = PathVerbKind::Close;
  p.verbs.push_back(v);
  return p;
}

DrawPath MakeLine(double x1, double y1, double x2, double y2, Rgba color, double width_pt) {
  DrawPath p;
  p.has_fill = false;
  p.has_stroke = true;
  p.stroke = color;
  p.stroke_pt = width_pt;
  PathVerb v;
  v.kind = PathVerbKind::MoveTo;
  v.x1 = x1;
  v.y1 = y1;
  p.verbs.push_back(v);
  v.kind = PathVerbKind::LineTo;
  v.x1 = x2;
  v.y1 = y2;
  p.verbs.push_back(v);
  return p;
}

}  // namespace doccore
