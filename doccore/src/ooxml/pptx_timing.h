// DocCore - 内部辅助：幻灯片的切换（p:transition）与动画（p:timing）。
// 仅供 src/ooxml/pptx_read.cpp 与 pptx_write.cpp 使用（不属于公开头文件）。
#pragma once

#include <string>
#include <vector>

#include "doccore/deck.h"

namespace doccore {
namespace ooxml {

// 从幻灯片 XML（p:sld）里解析切换与动画，填进 slide。
// 无法映射到 TransitionType 的切换类型会记进 unmapped（调用方转成告警）。
void ReadTransitionAndTiming(const std::string& xml, deck::Slide* slide,
                             std::vector<std::string>* unmapped);

// 生成 <p:transition> 片段；空串表示不写。
// slide.transition_xml() 非空时（读入时无法建模的原文）原样返回。
std::string TransitionXml(const deck::Slide& slide);

// 生成 <p:timing> 片段；空串表示不写。
// slide.timing_xml() 非空时原样返回，保住我们没建模的关键帧。
std::string TimingXml(const deck::Slide& slide);

}  // namespace ooxml
}  // namespace doccore
