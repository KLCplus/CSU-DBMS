#pragma once

#include "net/communicator.h"

/** CSUDB native protocol: one JSON object followed by a NUL byte. */
class NativeCommunicator final : public Communicator
{
public:
  RC read_event(SessionEvent *&event) override;
  RC write_result(SessionEvent *event, bool &need_disconnect) override;
  bool structured_protocol() const override { return true; }
  RC write_query_result(const QueryResult &result, bool &need_disconnect) override;

private:
  RC read_packet(string &packet);
};
