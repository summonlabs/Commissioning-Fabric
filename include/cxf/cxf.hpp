// Commissioning Fabric - umbrella header.
//
// Including this header pulls in the whole public surface. Including the
// individual headers is preferred in implementation files, because each layer's
// dependencies then stay visible at the point they are used.
#ifndef CXF_CXF_HPP
#define CXF_CXF_HPP

#include "cxf/codec/archive.hpp"
#include "cxf/codec/text_codec.hpp"
#include "cxf/model/candidate.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/model/lifecycle.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/persist/journal.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/persist/snapshot.hpp"
#include "cxf/runtime/authority.hpp"
#include "cxf/runtime/event.hpp"
#include "cxf/runtime/explain.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/runtime/plan.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/crc32c.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/lock_file.hpp"
#include "cxf/support/serial.hpp"
#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/tools/registry.hpp"
#include "cxf/tools/textlog.hpp"
#include "cxf/types/timepoint.hpp"

#endif  // CXF_CXF_HPP
