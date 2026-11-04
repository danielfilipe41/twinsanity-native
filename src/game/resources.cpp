#include "game/graphicstables.h"

#include "game/stream.h"
#include "platform/graphics.h"

GameTexture* TextureFromStream(u32 id, Stream* stream)
{
    TextureTable::Entry* entry = g_TextureTable.Find(id);
    GameTexture* texture = entry != nullptr ? entry->item : nullptr;
    if (texture == nullptr)
    {
        texture = Platform::Graphics::NewTexture(id);
        TakeReference(texture);
        g_TextureTable.Insert(&texture, id);
        Platform::Graphics::ReadTexture(texture, stream);
    }
    else if (stream != nullptr)
    {
        // The table had it, so nothing read the stream: the caller's next read is the bytes past this texture (a picture's
        // material, a font's next page), which are the item's own 4-byte size and then that many bytes -- exactly what
        // ReadTexture consumes. Stepping over them keeps the caller's stream where it expects to be; without it the material
        // behind the texture parsed from inside the texture's header, which came out an empty material with no shader and no
        // texture, cached that way until the picture was read again (the port's white level-title cards)
        u32 size;
        stream->ReadU32(&size);
        stream->Skip(static_cast<s32>(size));
    }

    return texture;
}

MaterialResource* MaterialFromStream(u32 id, Stream* stream)
{
    MaterialTable::Entry* entry = g_MaterialTable.Find(id);
    MaterialResource* material = entry != nullptr ? entry->item : nullptr;
    if (material == nullptr)
    {
        material = Platform::Graphics::NewMaterial(id);
        TakeReference(material);
        g_MaterialTable.Insert(&material, id);
        Platform::Graphics::ReadMaterial(material, stream);
    }

    return material;
}
