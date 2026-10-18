#include "d3d/hle_table_posix.h"

#include "d3d/thunk_posix.h"

namespace xd3d {

TableReport build_table(const Hle *rows, size_t count, std::vector<void *> *thunks)
{
    TableReport report;
    report.rows = count;
    if (thunks) {
        thunks->assign(count, nullptr);
        // The arena has to exist before any thunk is built. Passing the image size and base of zero says
        // "no image in particular", which maps an arena somewhere harmless -- enough for a test that only
        // wants to call through the thunks, and harmless in a run that never installs hooks.
    }

    for (size_t i = 0; i < count; i++) {
        std::string why;
        void *t = make_hle_thunk(rows[i], &why);
        if (t) {
            report.built++;
            if (thunks) (*thunks)[i] = t;
        } else {
            report.refused.push_back(TableReport::Refusal{rows[i].name ? rows[i].name : "?", rows[i].addr, why});
        }
    }
    return report;
}

} // namespace xd3d