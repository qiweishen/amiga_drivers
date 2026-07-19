#ifndef CRC_H
#define CRC_H 1

#include <QtGlobal>

/**
 * \brief Computes the CRC of the given buffer with given length.
 * \param buf
 * \param buf_length
 * \return
 */
quint16 CRC_compute16CCITT(const void* buf, size_t buf_length);


#endif
