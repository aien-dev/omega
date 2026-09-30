#include "aegis_verification.h"
#include "forge_realize.h"
#include <string.h>

int omega_aegis_verify(const OmegaRealizationRequest  *req,
                       const ForgeRealizationResult   *res,
                       const ForgeMachineDescriptor   *desc,
                       ForgeVerifiedRealization       *out_verified) {
    if (!req || !res || !desc || !out_verified) return -1;

    ForgeRealizationRequest f_req;
    memset(&f_req, 0, sizeof(f_req));
    memcpy(f_req.program_digest, req->program_digest, 32);
    f_req.opcode_count = req->opcode_count;
    f_req.ir_payload = req->ir_payload;
    f_req.ir_size = req->ir_size;
    f_req.target_machine = req->machine_desc;
    memcpy(f_req.target_descriptor_digest, req->descriptor_digest, 32);

    return aegis_verify_realization(&f_req, res, desc, out_verified);
}
