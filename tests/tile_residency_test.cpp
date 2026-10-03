#include "rendering/tile_residency.h"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace compositor;
using namespace compositor::engine;
namespace {
void check(bool value) { if(!value) throw std::runtime_error("Tile residency check failed"); }
template<class F> void rejects(F action) {
    bool rejected=false;
    try { action(); } catch(const std::logic_error&) { rejected=true; }
    check(rejected);
}
}
int main() {
    try {
        TileStore first(8*1024*1024),second(8*1024*1024);
        auto a=first.constant(256,256,{1,0,0,1});
        auto b=second.constant(256,256,{0,1,0,1});
        auto c=first.constant(1,3,{0,0,1,1});
        check(a->version()==b->version()); // Must not collide across stores.
        TileResidency pool(2*TileResidency::slotBytes+17);
        check(pool.capacity()==2);
        rejects([&]{pool.acquire(a);});
        rejects([&]{pool.begin(1);});
        pool.begin(0);
        rejects([&]{pool.begin(0);});
        rejects([&]{pool.acquire({});});
        auto la=pool.acquire(a),lb=pool.acquire(b);
        check(la && lb && la->upload && lb->upload && la->slot!=lb->slot);
        auto shared=pool.acquire(a);
        check(shared && !shared->upload && shared->slot==la->slot);
        check(!pool.acquire(c));
        rejects([&]{pool.submitted(0);});
        check(pool.pending());
        pool.submitted(2); // Timeline values may skip.
        pool.begin(0);
        check(!pool.acquire(c)); // Both slots still in flight.
        check(!pool.acquire(a)->upload); // Read/read reuse on the same queue.
        pool.submitted(3);
        pool.begin(2);
        auto lc=pool.acquire(c);
        check(lc && lc->upload && lc->slot==lb->slot); // a retained through 3.
        pool.cancel();
        check(pool.residentCount()==1); // Cancelled c was never uploaded.
        pool.begin(2);
        check(pool.acquire(c)->upload);
        pool.submitted(4);
        rejects([&]{pool.begin(1);});
        rejects([&]{pool.begin(5);});
        pool.begin(4);
        auto replaced=pool.acquire(b);
        check(replaced && replaced->slot==la->slot); // Oldest retired tile.
        pool.cancel();
        pool.begin(4);
        check(!pool.acquire(c)->upload);
        pool.cancel();
        rejects([&]{pool.cancel();});
        rejects([&]{pool.submitted(5);});
        TileResidency zero(TileResidency::slotBytes-1);
        zero.begin(0);
        check(!zero.acquire(a));
        zero.cancel();
        // Cache retains identity even after its document/store is destroyed;
        // cancellation releases never-submitted ownership without leaking it.
        TileResidency ownership(TileResidency::slotBytes);
        std::weak_ptr<const Tile> weak;
        ownership.begin(0);
        {
            TileStore temporary(1024*1024);
            auto tile=temporary.constant(2,2,{0,0,0,0});
            weak=tile;
            check(ownership.acquire(tile)->upload);
        }
        check(!weak.expired());
        ownership.cancel();
        check(weak.expired());
        // Deterministic queue simulation: emulate bytes in four GPU slots and
        // independently track their last submitted readers. Cache pressure,
        // cancellation and delayed retirement must never overwrite live data.
        TileResidency stress(4*TileResidency::slotBytes);
        std::vector<TilePtr> tiles;
        for(int i=0;i<17;++i)
            tiles.push_back(first.constant(1,1,{float(i)/32,0,0,1}));
        std::array<TilePtr,4> gpu{};
        std::array<std::uint64_t,4> readers{};
        std::uint64_t serial=0,complete=0;
        std::uint32_t random=12345;
        auto next=[&] { random=random*1664525u+1013904223u; return random; };
        for(int batch=0;batch<10000;++batch) {
            if(next()%3==0) complete=serial;
            stress.begin(complete);
            std::array<TilePtr,4> writes{},uses{};
            for(int request=0;request<7;++request) {
                auto tile=tiles[next()%tiles.size()];
                auto lease=stress.acquire(tile);
                if(!lease) continue;
                const auto slot=lease->slot;
                check(slot<gpu.size());
                if(lease->upload) {
                    check(readers[slot]<=complete);
                    check(!uses[slot]);
                    writes[slot]=tile;
                } else check((writes[slot] ? writes[slot] : gpu[slot])==tile);
                uses[slot]=tile;
            }
            if(next()%5==0) stress.cancel();
            else {
                ++serial;
                for(std::size_t slot=0;slot<gpu.size();++slot) {
                    if(writes[slot]) gpu[slot]=writes[slot];
                    if(uses[slot]) { check(gpu[slot]==uses[slot]); readers[slot]=serial; }
                }
                stress.submitted(serial);
            }
        }
        std::cout<<"Tile residency lifecycle passed\n";
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
