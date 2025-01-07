// -------------------------------------------------------------------------------------------------
// Copyright (c) Lukas Vik. All rights reserved.
//
// This file is part of the hdl-modules project, a collection of reusable, high-quality,
// peer-reviewed VHDL building blocks.
// https://hdl-modules.com
// https://github.com/hdl-modules/hdl-modules
// -------------------------------------------------------------------------------------------------

#pragma once

#include "dma_axi_write_simple.h"

namespace fpga {

namespace dma_axi_write_simple {

struct Response {
  size_t num_bytes;
  volatile void *data;
};

/**
 * Class with simple API for using the simple AXI DMA write FPGA module.
 * This class does not copy data from the memory buffer before passing it on to
 * the user.
 * This makes it very efficient but it also means that the user must keep track
 * of what they are doing with the data.
 * See the methods DmaNoCopy::received_data and DmaNoCopy::done_with_data for
 * details.
 */
class DmaNoCopy {

private:
  fpga_regs::DmaAxiWriteSimple *m_registers;

  volatile uint8_t *m_buffer;
  size_t m_buffer_size_bytes;

  bool (*m_assertion_handler)(const std::string *);

  uint32_t m_start_address;
  uint32_t m_end_address;
  uint32_t m_in_buffer_read_outstanding_address = 0;
  uint32_t m_in_buffer_read_done_address = 0;

  /**
   * Returns 'true' if the 'write_done' interrupt has triggered.
   * Will call an assertion if any of the error interrupts have triggered.
   */
  bool check_status();

public:
  /**
   * Class constructor.
   * @param dma_axi_write_simple_registers Register interface object pointer.
   * @param buffer Pointer to memory buffer.
   *               Must be allocated by user.
   *               The address must be aligned with the packet length used by
   *               the FPGA.
   *               Will not be deleted by this class in any destructor, etc.
   *
   *               Note that this constructor will use this buffer for both the
   *               physical and virtual memory address.
   *               Meaning this constructor is only suitable for bare
   *               metal applications.
   * @param buffer_size_bytes The number of bytes in the memory buffer.
   *                          I.e. the number of bytes that have been allocated
   *                          by the user for the 'buffer' argument.
   *                          Must be a multiple of the packet length used by
   *                          the FPGA.
   * @param assertion_handler Function to call when an assertion fails in
   *                          this class.
   *                          Function takes a string pointer as an argument and
   *                          must return a boolean 'true'.
   */
  DmaNoCopy(fpga_regs::DmaAxiWriteSimple *dma_axi_write_simple_registers,
            void *buffer, size_t buffer_size_bytes,
            bool (*assertion_handler)(const std::string *));

  /**
   * Write the necessary registers to setup the DMA module for operation, and
   * then enable it.
   * When this is done, streaming data in FPGA will start to be written to DDR
   * memory.
   */
  void setup_and_enable();

  /**
   * Receive all data that has been written by FPGA (no lower or upper limit for
   * byte count).
   * See documentation of DmaNoCopy::receive_data for more details.
   */
  Response receive_all_data();

  /**
   * Receive data that has been written to memory by FPGA, given the byte count
   * limits specified in the arguments.
   * Will return zero if no data is available yet.
   *
   * When data is read with this method it is considered outstanding, and the
   * part of the memory buffer where it resides will not be written by the FPGA
   * again.
   * Only once DmaNoCopy::done_with_data is called can there be further writes
   * there.
   * At that point, it is not safe to use the data pointer provided by a
   * previous call to this method.
   * At that point, the data must have been copied or you must be completely
   * done with it.
   *
   * Whenever this method is called and it returns non-zero, the data will be
   * considered outstanding.
   * That MUST be handled by the user and DmaNoCopy::done_with_data MUST
   * eventually be called.
   *
   * This method will check the current interrupt status, which will trigger an
   * assertion call if any error interrupt has occurred.
   *
   * @param min_num_bytes The minimum number of bytes we want to receive.
   *                      If fewer data bytes are available to read in memory,
   *                      the method will return zero.
   *
   *                      The value provided must be a multiple of the packet
   *                      length used by the FPGA.
   *
   *                      There is a corner case where this method can return a
   *                      number of bytes that is non-zero but less than this
   *                      argument.
   *                      If this argument is greater than the packet length
   *                      used by the FPGA, and the data we are returning is at
   *                      the end of the ring buffer, there is no way to return
   *                      this specified minimum number of bytes. This is
   *                      because this class performs no copying of the data, it
   *                      only provides it as it is in the ring buffer.
   *
   *                      This corner case must be taken into account by the
   *                      user by always inspecting the number of bytes in the
   *                      response.
   * @param max_num_bytes If more than this number of data bytes are available
   *                      to read in memory, the method will split it up and
   *                      return 'max_num_bytes' bytes from this call.
   *
   *                      The value provided must be a multiple of the packet
   *                      length used by the FPGA.
   */
  Response receive_data(size_t min_num_bytes, size_t max_num_bytes);

  /**
   * Indicate that we are done with data previously read with
   * DmaNoCopy::receive_data.
   * Will mark the corresponding buffer segments as free to be written to again
   * by FPGA.
   *
   * Do not call this method with an argument that is greater than the number
   * of bytes that has previously been read with DmaNoCopy::receive_data.
   *
   * Do not perform any 'delete' on the data.
   */
  void done_with_data(size_t num_bytes);

  /**
   * Clear all DMA data, which means
   * - Indicate to the FPGA that the whole memory buffer is free to be written.
   * - Reset the DmaNoCopy::receive_data/DmaNoCopy::done_with_data state,
   *   so that no data is considered outstanding.
   *
   * An implication of this is that if you have data that has been received with
   * DmaNoCopy::receive_data, but you are not yet finished with it and have
   * not called DmaNoCopy::done_with_data for it, that memory might be
   * overwritten by the FPGA.
   *
   * This method is not really meant to be used under regular circumstances.
   */
  void clear_all_data();

  /**
   * Return the number of bytes of data that is available for receiving
   * in the memory buffer.
   * This is data that has been written by the FPGA, but has not yet
   * been received by the software with e.g. DmaNoCopy::receive_data.
   *
   * Note that there is a duplicate register read in this one and
   * DmaNoCopy::receive_data.
   * Since register reads are usually quite slow, polling
   * with this method and then reading with DmaNoCopy::receive_data is not
   * recommended.
   * Instead, call DmaNoCopy::receive_data, either
   * - with the exact number of bytes you want as the arguments, or
   * - with a range and then check how much data you got as a response.
   */
  size_t get_num_bytes_available();

private:
  // Empty struct initialization -> all fields zero'd out.
  // (most importantly, the 'num_bytes' value).
  const Response response_zero_bytes = {};
};

} // namespace dma_axi_write_simple

} // namespace fpga
