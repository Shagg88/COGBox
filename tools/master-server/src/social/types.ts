// Social types shared by the friends API, presence and the push events (social design step 2).
export interface Presence { state: 'offline' | 'online' | 'playing' | 'joinable'; session_id?: string; room_code?: string; title_id?: string; lobby_id?: string }
export interface FriendRef { uuid: string; tag: string }
export interface Friend extends FriendRef { xuid: string; presence: Presence }   // xuid: the games' XFriends key
