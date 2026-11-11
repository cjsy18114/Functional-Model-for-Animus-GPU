#include "model/submission.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace animus {
namespace {
// ReferenceV1 geometry is independent of guest frm and restores the host's
// environment. Build with FP contraction disabled and rounding-math enabled.
class GeometryScope {
    std::fenv_t saved_{};
    bool saved_ok_ = false, ready_ = false;
public:
    GeometryScope() { saved_ok_ = std::fegetenv(&saved_) == 0; ready_ = saved_ok_ && std::fesetround(FE_TONEAREST) == 0; }
    ~GeometryScope() { if (saved_ok_) std::fesetenv(&saved_); }
    bool ready() const { return ready_; }
};
Issue geometry_limit(const char* code, const char* message) {
    auto issue=make_issue(IssueKind::ModelLimit,code,message); issue.spec_issue_id="S09"; return issue;
}
Issue environment_error() { return make_issue(IssueKind::HostError,"GeometryEnvironment","Cannot establish host RNE geometry environment."); }
double plane_distance(const RasterVertex& vertex, unsigned plane) {
    const auto& p=vertex.position;
    switch (plane) {
    case 0: return p[0]+p[3]; case 1: return p[3]-p[0];
    case 2: return p[1]+p[3]; case 3: return p[3]-p[1];
    case 4: return p[2]; default: return p[3]-p[2];
    }
}
std::int64_t edge(std::int64_t ax,std::int64_t ay,std::int64_t bx,std::int64_t by,std::int64_t px,std::int64_t py) {
    return (bx-ax)*(py-ay)-(by-ay)*(px-ax);
}
bool top_left(std::int64_t ax,std::int64_t ay,std::int64_t bx,std::int64_t by) {
    return by<ay || (by==ay && bx>ax); // Interior-positive edges, Y-down framebuffer.
}
std::int64_t floor_div(std::int64_t value,std::int64_t divisor) {
    auto q=value/divisor; if (value%divisor<0) --q; return q;
}
bool fp32_less(Word a,Word b) {
    if ((a&0x7FFFFFFFu)>0x7F800000u || (b&0x7FFFFFFFu)>0x7F800000u) return false;
    if ((a&0x7FFFFFFFu)==0 && (b&0x7FFFFFFFu)==0) return false;
    if ((a>>31)!=(b>>31)) return (a>>31)!=0;
    return (a>>31) ? a>b : a<b;
}
}
Result<std::vector<Triangle>> clip_triangle(const std::array<VertexOutput,3>& input,Word primitive_id,unsigned varying_count) {
    GeometryScope scope; if (!scope.ready()) return environment_error();
    if (varying_count>8) return make_issue(IssueKind::HostError,"InvalidVaryingCount","Invalid clipping varying count.");
    std::vector<RasterVertex> polygon;
    for (const auto& vertex : input) {
        RasterVertex out;
        for (unsigned k=0;k<4;++k) {
            out.position[k]=static_cast<double>(std::bit_cast<float>(vertex.position_bits[k]));
            if (!std::isfinite(out.position[k])) return geometry_limit("NonFinitePosition","ReferenceV1 cannot yet classify nonfinite clip positions.");
        }
        for (unsigned j=0;j<varying_count;++j) out.varying[j]=static_cast<double>(std::bit_cast<float>(vertex.varying_bits[j]));
        polygon.push_back(out);
    }
    // Six planes in fixed order; inside includes the plane. Canonical endpoint
    // ordering makes a shared edge's intersection independent of traversal direction.
    for (unsigned plane=0;plane<6 && !polygon.empty();++plane) {
        std::vector<RasterVertex> next;
        for (std::size_t i=0;i<polygon.size();++i) {
            const auto& a=polygon[i]; const auto& b=polygon[(i+1)%polygon.size()];
            const auto da=plane_distance(a,plane), db=plane_distance(b,plane);
            if (da>=0) next.push_back(a);
            if ((da>=0)!=(db>=0)) {
                const bool forward=a.position<b.position;
                const auto& lo=forward?a:b; const auto& hi=forward?b:a;
                const auto dl=forward?da:db, dh=forward?db:da;
                const double t=dl/(dl-dh);
                RasterVertex intersection;
                for (unsigned k=0;k<4;++k) intersection.position[k]=lo.position[k]+t*(hi.position[k]-lo.position[k]);
                for (unsigned j=0;j<varying_count;++j) intersection.varying[j]=lo.varying[j]+t*(hi.varying[j]-lo.varying[j]);
                next.push_back(intersection);
            }
        }
        polygon=std::move(next);
    }
    std::vector<Triangle> triangles;
    for (std::size_t i=1;i+1<polygon.size();++i) triangles.push_back({{polygon[0],polygon[i],polygon[i+1]},primitive_id});
    return triangles;
}
Result<std::optional<RasterTriangle>> setup_triangle(const Triangle& input,const GraphicsState& state) {
    GeometryScope scope; if (!scope.ready()) return environment_error();
    if (state.varying_count>8 || state.width==0 || state.height==0)
        return make_issue(IssueKind::HostError,"InvalidRasterState","Setup requires validated dimensions and varying count.");
    RasterTriangle raster; raster.primitive_id=input.primitive_id;
    auto vertices=input.vertices;
    for (unsigned i=0;i<3;++i) {
        const auto& p=vertices[i].position;
        if (p[3]<=0 || !std::isfinite(p[3])) return geometry_limit("ProjectionSingularity","Clipped geometry still has nonpositive/invalid w; no epsilon or silent discard is assumed.");
        const double nx=p[0]/p[3], ny=p[1]/p[3];
        const double sx=state.viewport_x+(nx+1.0)*(static_cast<double>(state.viewport_width)*0.5);
        const double sy=state.viewport_y+(ny+1.0)*(static_cast<double>(state.viewport_height)*0.5);
        const double qx=std::nearbyint(sx*256.0), qy=std::nearbyint(sy*256.0);
        // Bounds guarantee every difference/product/sum in edge() fits int64.
        if (!std::isfinite(qx) || !std::isfinite(qy) || std::abs(qx)>536870912.0 || std::abs(qy)>536870912.0)
            return geometry_limit("RasterIntegerRange","Geometry exceeds ReferenceV1 checked integer coverage capacity.");
        raster.x[i]=static_cast<std::int64_t>(qx); raster.y[i]=static_cast<std::int64_t>(qy);
    }
    auto area=edge(raster.x[0],raster.y[0],raster.x[1],raster.y[1],raster.x[2],raster.y[2]);
    if (area==0 || ((state.flags&4u) && area>0)) return std::optional<RasterTriangle>{};
    // Visual CCW is area<0 in a Y-down framebuffer. Normalize only after culling.
    if (area<0) { std::swap(raster.x[1],raster.x[2]); std::swap(raster.y[1],raster.y[2]); std::swap(vertices[1],vertices[2]); area=-area; }
    raster.area=area;
    const auto minx=*std::min_element(raster.x.begin(),raster.x.end()), maxx=*std::max_element(raster.x.begin(),raster.x.end());
    const auto miny=*std::min_element(raster.y.begin(),raster.y.end()), maxy=*std::max_element(raster.y.begin(),raster.y.end());
    const auto left=std::max({std::int64_t{0},-floor_div(-(minx-128),256),std::int64_t{state.scissor_x}});
    const auto top=std::max({std::int64_t{0},-floor_div(-(miny-128),256),std::int64_t{state.scissor_y}});
    const auto right=std::min({std::int64_t{state.width}-1,floor_div(maxx-128,256),std::int64_t{state.scissor_x}+state.scissor_width-1});
    const auto bottom=std::min({std::int64_t{state.height}-1,floor_div(maxy-128,256),std::int64_t{state.scissor_y}+state.scissor_height-1});
    if (left>right || top>bottom) return std::optional<RasterTriangle>{};
    raster.left=static_cast<int>(left); raster.right=static_cast<int>(right);
    raster.top=static_cast<int>(top); raster.bottom=static_cast<int>(bottom);
    for (unsigned i=0;i<3;++i) {
        const double inv=1.0/vertices[i].position[3];
        raster.depth[i]=vertices[i].position[2]/vertices[i].position[3];
        raster.inv_w[i]=reference_f32(inv);
        for (unsigned j=0;j<state.varying_count;++j) raster.attr_over_w[j][i]=reference_f32(vertices[i].varying[j]*inv);
    }
    return std::optional<RasterTriangle>{raster};
}
Result<Addr> write_interp_record(Machine& machine,const RasterTriangle& triangle,unsigned varying_count) {
    if (varying_count>8) return make_issue(IssueKind::HostError,"InvalidVaryingCount","Invalid interpolation record size.");
    const std::uint64_t bytes=12+12*varying_count;
    for (const auto& resource : machine.resources) {
        if (resource.role!=ResourceRole::Scratch || resource.bytes<bytes) continue;
        auto readable=require_resource(machine,ResourceRole::Scratch,resource.base,bytes,Access::Read);
        if (!readable) return readable.error();
        auto writable=require_resource(machine,ResourceRole::Scratch,resource.base,bytes,Access::Write);
        if (!writable) return writable.error();
        for (unsigned i=0;i<3;++i) machine.memory.store_le_prechecked(resource.base+i*4,4,triangle.inv_w[i]);
        for (unsigned j=0;j<varying_count;++j) for (unsigned i=0;i<3;++i)
            machine.memory.store_le_prechecked(resource.base+12+j*12+i*4,4,triangle.attr_over_w[j][i]);
        return resource.base;
    }
    auto issue=make_issue(IssueKind::ModelLimit,"ScratchCapacity","No disjoint host scratch range fits the interpolation record.");
    issue.spec_issue_id="S10.4"; return issue;
}
Result<std::optional<InvocationInput>> raster_sample(const RasterTriangle& triangle,const GraphicsState& state,Addr interpolation,std::uint64_t index) {
    GeometryScope scope; if (!scope.ready()) return environment_error();
    if (triangle.area<=0 || triangle.right<triangle.left || triangle.bottom<triangle.top ||
        index>=std::uint64_t(triangle.right-triangle.left+1)*std::uint64_t(triangle.bottom-triangle.top+1))
        return make_issue(IssueKind::HostError,"InvalidRasterSample","Sample requires a valid setup and in-range scan index.");
    const auto width=static_cast<std::uint64_t>(triangle.right-triangle.left+1);
    const auto x=triangle.left+static_cast<int>(index%width), y=triangle.top+static_cast<int>(index/width);
    const std::int64_t px=std::int64_t{x}*256+128, py=std::int64_t{y}*256+128;
    std::array<std::int64_t,3> values{};
    for (unsigned i=0;i<3;++i) {
        const auto a=(i+1)%3,b=(i+2)%3;
        values[i]=edge(triangle.x[a],triangle.y[a],triangle.x[b],triangle.y[b],px,py);
        if (values[i]<0 || (values[i]==0 && !top_left(triangle.x[a],triangle.y[a],triangle.x[b],triangle.y[b])))
            return std::optional<InvocationInput>{};
    }
    const double l0=static_cast<double>(values[0])/static_cast<double>(triangle.area);
    const double l1=static_cast<double>(values[1])/static_cast<double>(triangle.area);
    const double l2=static_cast<double>(values[2])/static_cast<double>(triangle.area);
    const double depth=(l0*triangle.depth[0]+l1*triangle.depth[1])+l2*triangle.depth[2];
    InvocationInput input;
    input.pixel_x=static_cast<Word>(x); input.pixel_y=static_cast<Word>(y);
    input.uniform_base=state.uniform_base; input.primitive_id=triangle.primitive_id; input.prim_data_addr=interpolation;
    input.depth_bits=reference_f32(std::clamp(depth,0.0,1.0));
    input.bary0_bits=reference_f32(l0); input.bary1_bits=reference_f32(l1);
    return std::optional<InvocationInput>{input};
}
Result<void> merge_fragment(Machine& machine,const FragmentOutput& fragment) {
    const auto& s=machine.submission.graphics; const auto& input=fragment.input;
    if (input.pixel_x>=s.width || input.pixel_y>=s.height)
        return make_issue(IssueKind::HostError,"InvalidFragmentCoordinates","Internal fragment escaped attachment bounds.");
    const auto color_address=static_cast<Addr>(std::uint64_t{s.color_base}+std::uint64_t{input.pixel_y}*s.color_stride+std::uint64_t{input.pixel_x}*4);
    const auto depth_address=static_cast<Addr>(std::uint64_t{s.depth_base}+std::uint64_t{input.pixel_y}*s.depth_stride+std::uint64_t{input.pixel_x}*4);
    if (s.flags&1u) {
        auto resource=require_resource(machine,ResourceRole::Depth,depth_address,4,Access::Read); if (!resource) return resource.error();
        if (!fp32_less(input.depth_bits,machine.memory.peek_le_prechecked(depth_address,4))) return {};
    }
    auto color=require_resource(machine,ResourceRole::Color,color_address,4,Access::Write); if (!color) return color.error();
    if (s.flags&2u) { auto depth=require_resource(machine,ResourceRole::Depth,depth_address,4,Access::Write); if (!depth) return depth.error(); }
    machine.memory.store_le_prechecked(color_address,4,fragment.rgba8);
    if (s.flags&2u) machine.memory.store_le_prechecked(depth_address,4,input.depth_bits);
    return {};
}
} // namespace animus
