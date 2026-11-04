#pragma once
// Writes the details of a fatal CPU exception (address, registers, likely callers) to
// fable_online.log, so a crash or freeze in the field can be traced to a code address.
namespace fo::crash {
void install();
}
