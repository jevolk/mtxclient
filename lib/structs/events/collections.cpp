#include "mtx/events/collections.hpp"
#include "mtx/log.hpp"

namespace mtx::events::collections {
void
from_json(const json &obj, TimelineEvent &e)
{
        const auto type = mtx::events::getEventType(obj);
        using namespace mtx::events::state;
        using namespace mtx::events::msg;

        switch (type) {
        case events::EventType::RoomAliases: {
                e.data = events::StateEvent<Aliases>(obj);
                break;
        }
        case events::EventType::RoomAvatar: {
                e.data = events::StateEvent<Avatar>(obj);
                break;
        }
        case events::EventType::RoomCanonicalAlias: {
                e.data = events::StateEvent<CanonicalAlias>(obj);
                break;
        }
        case events::EventType::RoomCreate: {
                e.data = events::StateEvent<Create>(obj);
                break;
        }
        case events::EventType::RoomEncrypted: {
                e.data = events::EncryptedEvent<mtx::events::msg::Encrypted>(obj);
                break;
        }
        case events::EventType::RoomEncryption: {
                e.data = events::StateEvent<Encryption>(obj);
                break;
        }
        case events::EventType::RoomGuestAccess: {
                e.data = events::StateEvent<GuestAccess>(obj);
                break;
        }
        case events::EventType::RoomHistoryVisibility: {
                e.data = events::StateEvent<HistoryVisibility>(obj);
                break;
        }
        case events::EventType::RoomJoinRules: {
                e.data = events::StateEvent<JoinRules>(obj);
                break;
        }
        case events::EventType::RoomMember: {
                e.data = events::StateEvent<Member>(obj);
                break;
        }
        case events::EventType::RoomName: {
                e.data = events::StateEvent<Name>(obj);
                break;
        }
        case events::EventType::RoomPowerLevels: {
                e.data = events::StateEvent<PowerLevels>(obj);
                break;
        }
        case events::EventType::RoomRedaction: {
                e.data = events::RedactionEvent<mtx::events::msg::Redaction>(obj);
                break;
        }
        case events::EventType::RoomTombstone: {
                e.data = events::StateEvent<Tombstone>(obj);
                break;
        }
        case events::EventType::RoomTopic: {
                e.data = events::StateEvent<Topic>(obj);
                break;
        }
        case events::EventType::RoomMessage: {
                using MsgType       = mtx::events::MessageType;
                const auto msg_type = mtx::events::getMessageType(obj.at("content"));

                if (msg_type == events::MessageType::Unknown) {
                        try {
                                auto unsigned_data =
                                  obj.at("unsigned").at("redacted_by").get<std::string>();

                                if (unsigned_data.empty())
                                        return;

                                e.data = events::RoomEvent<events::msg::Redacted>(obj);
                                return;
                        } catch (json::exception &err) {
                                mtx::utils::log::log_error(std::string("Invalid event type: ") +
                                                           err.what() + " " + obj.dump(2));
                                return;
                        }
                }

                switch (msg_type) {
                case MsgType::Audio: {
                        e.data = events::RoomEvent<events::msg::Audio>(obj);
                        break;
                }
                case MsgType::Emote: {
                        e.data = events::RoomEvent<events::msg::Emote>(obj);
                        break;
                }
                case MsgType::File: {
                        e.data = events::RoomEvent<events::msg::File>(obj);
                        break;
                }
                case MsgType::Image: {
                        e.data = events::RoomEvent<events::msg::Image>(obj);
                        break;
                }
                case MsgType::Location: {
                        /* events::RoomEvent<events::msg::Location> location = e; */
                        /* container.emplace_back(location); */
                        break;
                }
                case MsgType::Notice: {
                        e.data = events::RoomEvent<events::msg::Notice>(obj);
                        break;
                }
                case MsgType::Text: {
                        e.data = events::RoomEvent<events::msg::Text>(obj);
                        break;
                }
                case MsgType::Video: {
                        e.data = events::RoomEvent<events::msg::Video>(obj);
                        break;
                }
                case MsgType::Unknown:
                        return;
                }
                break;
        }
        case events::EventType::Sticker: {
                e.data = events::Sticker(obj);
                break;
        }
        case events::EventType::RoomPinnedEvents:
        case events::EventType::RoomKeyRequest: // Not part of the timeline
        case events::EventType::Tag:            // Not part of the timeline
        case events::EventType::PushRules:      // Not part of the timeline
        case events::EventType::KeyVerificationCancel:
        case events::EventType::KeyVerificationRequest:
        case events::EventType::KeyVerificationStart:
        case events::EventType::KeyVerificationAccept:
        case events::EventType::KeyVerificationKey:
        case events::EventType::KeyVerificationMac:
        case events::EventType::Unsupported:
                return;
        }
}
}
