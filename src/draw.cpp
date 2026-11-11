#include "model/submission.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace animus {
namespace {
Result<WarpId> launch_shader(Machine& machine,ShaderStage stage,std::span<const InvocationInput> inputs) {
    const auto& state=machine.submission.graphics;
    LaunchInfo launch; launch.entry=stage==ShaderStage::Vertex ? state.vs_entry : state.fs_entry;
    launch.init_mask=width_mask(static_cast<unsigned>(inputs.size()));
    auto shader=require_resource(machine,ResourceRole::Shader,launch.entry,4,Access::Execute);
    if (!shader) return shader.error();
    if ((shader.value().base&3u) || (shader.value().bytes&3u))
        return make_issue(IssueKind::HostError,"ShaderResourceAlignment","Host shader resource must contain complete aligned instructions.");
    launch.code_regions.push_back({shader.value().base,shader.value().bytes});
    auto uniform=require_resource(machine,ResourceRole::Uniform,state.uniform_base,1,Access::Read);
    if (!uniform) return uniform.error();
    launch.dispatch.stage=stage; launch.dispatch.varying_count=state.varying_count; launch.dispatch.graphics=state;
    std::copy(inputs.begin(),inputs.end(),launch.dispatch.inputs.begin());
    auto created=create_warp(machine,launch); if (!created) return created.error();
    if (!created.value()) return make_issue(IssueKind::HostError,"EmptyShaderBatch","Internal dispatch must be nonempty.");
    return *created.value();
}
Result<bool> run_owned_warp(Machine& machine,DrawProgress& draw,std::ostream* trace,bool vertex) {
    auto record=step_shader(machine,*draw.warp);
    if (!record) return record.error();
    if (trace) write_trace(*trace,record.value());
    const auto& warp=machine.warps[*draw.warp];
    if (warp.status!=WarpStatus::Ended) return false;
    if (vertex) {
        const auto& vertices=warp.completed_outputs->vertices;
        draw.vertices.insert(draw.vertices.end(),vertices.begin(),vertices.end());
        draw.vertices_done+=static_cast<unsigned>(vertices.size());
    } else {
        draw.outputs=warp.completed_outputs->fragments; draw.merge_index=0;
    }
    auto snapshot=release_warp(machine,*draw.warp); if (!snapshot) return snapshot.error();
    draw.warp.reset(); return true;
}
}
Result<bool> advance_draw(Machine& machine,std::ostream* trace) {
    auto& submission=machine.submission; const auto& state=submission.graphics;
    if (!submission.draw) {
        const auto& command=*submission.current;
        if (command.b%3) return make_issue(IssueKind::ArchitecturalFault,"InvalidDrawCount","Triangle-list vertex count must be a multiple of three.");
        if (command.b==0) return true;
        if (std::uint64_t{command.a}+command.b-1>0xFFFFFFFFull)
            return make_issue(IssueKind::ArchitecturalFault,"VertexAddressOverflow","Vertex IDs exceed 32 bits.");
        if (machine.config.raster_profile!=RasterProfile::ReferenceV1) {
            auto issue=make_issue(IssueKind::ModelLimit,"RasterProfileRequired","Select explicit ReferenceV1 to execute a nonempty DRAW."); issue.spec_issue_id="S09"; return issue;
        }
        DrawProgress progress; progress.first=command.a; progress.count=command.b;
        submission.draw=std::move(progress);
    }
    auto& draw=*submission.draw;
    switch (draw.phase) {
    case DrawPhase::LaunchVertices: {
        if (draw.primitive==draw.count/3) return true;
        auto work=consume_work(machine); if (!work) return work.error();
        const unsigned count=std::min(machine.config.warp_width,3-draw.vertices_done);
        std::array<InvocationInput,3> inputs{};
        for (unsigned lane=0;lane<count;++lane) {
            auto& input=inputs[lane];
            const auto id=std::uint64_t{draw.first}+std::uint64_t{draw.primitive}*3+draw.vertices_done+lane;
            const auto address=std::uint64_t{state.vertex_base}+id*state.vertex_stride;
            if (address>0xFFFFFFFFull) return make_issue(IssueKind::ArchitecturalFault,"VertexAddressOverflow","Vertex address arithmetic overflowed.");
            auto resource=require_resource(machine,ResourceRole::Vertex,static_cast<Addr>(address),1,Access::Read); if (!resource) return resource.error();
            input.vertex_id=static_cast<Word>(id); input.vertex_addr=static_cast<Addr>(address); input.uniform_base=state.uniform_base;
        }
        auto launched=launch_shader(machine,ShaderStage::Vertex,std::span(inputs.data(),count)); if (!launched) return launched.error();
        draw.warp=launched.value(); draw.phase=DrawPhase::RunVertices; break;
    }
    case DrawPhase::RunVertices: {
        auto done=run_owned_warp(machine,draw,trace,true); if (!done) return done.error();
        if (done.value()) draw.phase=draw.vertices_done==3 ? DrawPhase::PrepareTriangles : DrawPhase::LaunchVertices;
        break;
    }
    case DrawPhase::PrepareTriangles: {
        auto work=consume_work(machine); if (!work) return work.error();
        const std::array<VertexOutput,3> vertices={draw.vertices[0],draw.vertices[1],draw.vertices[2]};
        auto clipped=clip_triangle(vertices,draw.primitive,state.varying_count); if (!clipped) return clipped.error();
        draw.triangles=std::move(clipped.value()); draw.triangle_index=0; draw.phase=DrawPhase::SetupTriangle; break;
    }
    case DrawPhase::SetupTriangle: {
        if (draw.triangle_index==draw.triangles.size()) {
            ++submission.completed_primitives; ++draw.primitive; draw.vertices_done=0; draw.vertices.clear();
            draw.phase=DrawPhase::LaunchVertices; break;
        }
        auto work=consume_work(machine); if (!work) return work.error();
        auto raster=setup_triangle(draw.triangles[draw.triangle_index],state); if (!raster) return raster.error();
        if (!raster.value()) { ++draw.triangle_index; break; }
        auto address=write_interp_record(machine,*raster.value(),state.varying_count); if (!address) return address.error();
        draw.raster=std::move(*raster.value()); draw.interpolation_address=address.value(); draw.scan_index=0;
        draw.fragments.clear(); draw.phase=DrawPhase::Scan; break;
    }
    case DrawPhase::Scan: {
        const auto& raster=*draw.raster;
        const auto total=std::uint64_t(raster.right-raster.left+1)*std::uint64_t(raster.bottom-raster.top+1);
        if (draw.scan_index==total || draw.fragments.size()==machine.config.warp_width) {
            if (draw.fragments.empty()) { ++draw.triangle_index; draw.raster.reset(); draw.phase=DrawPhase::SetupTriangle; break; }
            auto launched=launch_shader(machine,ShaderStage::Fragment,draw.fragments); if (!launched) return launched.error();
            draw.warp=launched.value(); draw.phase=DrawPhase::RunFragments; break;
        }
        auto work=consume_work(machine); if (!work) return work.error();
        auto sample=raster_sample(raster,state,draw.interpolation_address,draw.scan_index); if (!sample) return sample.error();
        if (sample.value()) draw.fragments.push_back(*sample.value());
        ++draw.scan_index; break;
    }
    case DrawPhase::RunFragments: {
        auto done=run_owned_warp(machine,draw,trace,false); if (!done) return done.error();
        if (done.value()) draw.phase=DrawPhase::Merge;
        break;
    }
    case DrawPhase::Merge: {
        if (draw.merge_index==draw.outputs.size()) { draw.outputs.clear(); draw.fragments.clear(); draw.phase=DrawPhase::Scan; break; }
        auto work=consume_work(machine); if (!work) return work.error();
        auto merged=merge_fragment(machine,draw.outputs[draw.merge_index]); if (!merged) return merged.error();
        ++draw.merge_index; ++submission.merged_fragments; break;
    }
    }
    return false;
}
} // namespace animus
