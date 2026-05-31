/*
 * of_error.h -- Unified error codes for openfpgaOS
 *
 * The negative numeric values match the RISC-V SBI error code enumeration
 * (https://github.com/riscv-non-isa/riscv-sbi-doc) so the openfpgaOS
 * vendor extensions slot cleanly into a standard SBI client.
 */

#ifndef OF_ERROR_H
#define OF_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OF_OK                    =  0,  /* SBI_SUCCESS */
    OF_ERR_FAILED            = -1,  /* SBI_ERR_FAILED            generic */
    OF_ERR_NOT_SUPPORTED     = -2,  /* SBI_ERR_NOT_SUPPORTED     no such EID/FID */
    OF_ERR_INVALID_PARAM     = -3,  /* SBI_ERR_INVALID_PARAM */
    OF_ERR_DENIED            = -4,  /* SBI_ERR_DENIED            permission */
    OF_ERR_INVALID_ADDRESS   = -5,  /* SBI_ERR_INVALID_ADDRESS */
    OF_ERR_ALREADY_AVAILABLE = -6,
    OF_ERR_ALREADY_STARTED   = -7,
    OF_ERR_ALREADY_STOPPED   = -8,
    OF_ERR_NO_SHMEM          = -9,
    OF_ERR_INVALID_STATE     = -10,
    OF_ERR_BAD_RANGE         = -11,
    OF_ERR_TIMEOUT           = -12,
    OF_ERR_IO                = -13,

    /* openfpgaOS-specific extensions (numeric range below the SBI set
     * to avoid future collisions if the SBI working group adds more). */
    OF_ERR_BUSY              = -100,
} of_error_t;

#ifdef __cplusplus
}
#endif

#endif /* OF_ERROR_H */
