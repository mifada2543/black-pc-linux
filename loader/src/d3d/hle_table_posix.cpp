#include "d3d/hle_table_posix.h"

#include "d3d/thunk_posix.h"
#include "input/xinput_posix.h"

#include <vector>

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

const Hle *input_rows(size_t *count)
{
    return xinput::hle_input_table(count);
}

void get_all_hle_rows(const Hle **rows, size_t *count)
{
    static std::vector<Hle> combined;
    if (combined.empty()) {
        size_t c;
        const Hle *r;

        r = device_rows(&c);
        combined.insert(combined.end(), r, r + c);
        r = resource_rows(&c);
        combined.insert(combined.end(), r, r + c);
        r = draw_rows(&c);
        combined.insert(combined.end(), r, r + c);
        r = input_rows(&c);
        combined.insert(combined.end(), r, r + c);
    }
    *rows = combined.data();
    *count = combined.size();
}

} // namespace xd3d