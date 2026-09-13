"""Inspect a RenderDoc capture offline. Driven by inspect-capture.sh.

Runs inside qrenderdoc's embedded interpreter, which is where the replay API
lives. Arguments arrive through the environment because qrenderdoc --python
takes no arguments of its own.

Capturing costs nothing at runtime and the capture can be read as many times as
needed, which is why this is the tool of choice for anything visual here:
hooks on hot guest paths starve the title, and debugger breakpoints freeze it
mid-frame and trip the driver's reset.
"""

import os
import struct
import sys
import traceback

import renderdoc as rd

OUT = open(os.environ["XE_INSPECT_OUT"], "w")


def say(*parts):
    OUT.write(" ".join(str(part) for part in parts) + "\n")


def walk(actions):
    for action in actions:
        yield action
        for child in walk(action.children):
            yield child


def texture_note(textures, resource):
    texture = textures.get(resource)
    if texture is None:
        return str(resource)
    note = "%s %dx%d %s" % (resource, texture.width, texture.height, texture.format.Name())
    if getattr(texture, "msSamp", 1) > 1:
        note += " x%d samples" % texture.msSamp
    return note


def sample_point(texture, fraction_x, fraction_y):
    """Where to probe a target, given a point as a fraction of the frame.

    The guest's colour targets are backed by one tall surface - 1280x2048 for a
    1280x720 frame - so the visible part is the top of it, and a fraction of the
    full height would land outside the picture.
    """
    height = texture.height
    if height > 1024 and texture.width <= 1280:
        height = 720
    return (min(int(texture.width * fraction_x), texture.width - 1),
            min(int(height * fraction_y), height - 1))


def describe_draw(controller, textures, event, with_shaders):
    controller.SetFrameEvent(event, True)
    state = controller.GetPipelineState()
    say("=" * 70)
    say("event", event)

    for index, target in enumerate(state.GetOutputTargets()):
        if target.resource != rd.ResourceId.Null():
            say("  target %d: %s" % (index, texture_note(textures, target.resource)))

    try:
        blends = state.GetColorBlends()
        if blends:
            blend = blends[0]
            say("  blend: on=%s mask=%X  colour %s %s %s  alpha %s %s %s" % (
                blend.enabled, blend.writeMask,
                blend.colorBlend.source, blend.colorBlend.destination, blend.colorBlend.operation,
                blend.alphaBlend.source, blend.alphaBlend.destination, blend.alphaBlend.operation))
    except Exception:
        pass

    for stage, name in ((rd.ShaderStage.Vertex, "vertex"), (rd.ShaderStage.Pixel, "pixel")):
        try:
            reflection = state.GetShaderReflection(stage)
        except Exception:
            continue
        if reflection is None:
            continue
        say("  -- %s shader --" % name)
        try:
            for entry in state.GetReadOnlyResources(stage):
                descriptor = getattr(entry, "descriptor", entry)
                resource = getattr(descriptor, "resource", None)
                if resource is not None and resource != rd.ResourceId.Null():
                    say("    texture:", texture_note(textures, resource))
        except Exception:
            pass
        for index, block in enumerate(reflection.constantBlocks):
            try:
                descriptor = state.GetConstantBlock(stage, index, 0)
                descriptor = getattr(descriptor, "descriptor", descriptor)
                if descriptor.resource == rd.ResourceId.Null():
                    continue
                size = min(descriptor.byteSize or 512, 1024)
                data = bytes(controller.GetBufferData(descriptor.resource,
                                                      descriptor.byteOffset, size))
                words = len(data) // 4
                values = struct.unpack("<%df" % words, data[:words * 4])
                say("    %s (%d bytes)" % (block.name, len(data)))
                if "system" in block.name:
                    # The fields that decide how colour leaves a draw: the
                    # exponent bias applied to each render target, and the flags
                    # saying how each target is packed - fixed point, float or
                    # gamma. A target treated as the wrong one crushes darks and
                    # clips highlights, which is what a badly exposed frame
                    # looks like.
                    floats = struct.unpack_from("<4f", data, 288)
                    formats = struct.unpack_from("<4I", data, 352)
                    say("      color_exp_bias   %.6f %.6f %.6f %.6f" % floats)
                    say("      rt_format_flags  %08X %08X %08X %08X" % formats)
                    say("      rt_base_dwords   %d %d %d %d"
                        % struct.unpack_from("<4I", data, 336))
                    say("      flags            %08X" % struct.unpack_from("<I", data, 0))
                    continue
                for at in range(0, min(words, int(os.environ.get("XE_CONSTANTS", "12")) * 4), 4):
                    say("      c%-3d %12.6f %12.6f %12.6f %12.6f"
                        % ((at // 4,) + values[at:at + 4]))
            except Exception:
                pass
        if with_shaders:
            try:
                say("    -- disassembly --")
                say(controller.DisassembleShader(state.GetGraphicsPipelineObject(),
                                                 reflection, ""))
            except Exception as error:
                say("    (no disassembly: %s)" % error)


def main():
    path = os.environ["XE_CAPTURE"]
    mode = os.environ.get("XE_MODE", "list")

    capture = rd.OpenCaptureFile()
    result = capture.OpenFile(path, "rdc", None)
    if not result.OK():
        raise SystemExit("cannot open %s: %s" % (path, result))
    result, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    if not result.OK():
        raise SystemExit("cannot replay %s: %s" % (path, result))

    textures = {texture.resourceId: texture for texture in controller.GetTextures()}
    actions = list(walk(controller.GetRootActions()))
    draws = [action for action in actions if action.flags & rd.ActionFlags.Drawcall]
    say("%d actions, %d draws" % (len(actions), len(draws)))

    if mode == "list":
        previous = None
        for action in draws:
            controller.SetFrameEvent(action.eventId, True)
            targets = controller.GetPipelineState().GetOutputTargets()
            target = targets[0].resource if targets else rd.ResourceId.Null()
            if target != previous:
                say("  from event %-5d -> %s" % (action.eventId,
                                                 texture_note(textures, target)))
                previous = target

    elif mode == "draws":
        # One line per draw: what it binds and how much geometry it has, which
        # is how a particular object is found when probing a pixel will not do -
        # the guest's colour targets are backed by the EDRAM surface, whose
        # layout is nothing like the picture.
        least = int(os.environ.get("XE_MIN_INDICES", "0"))
        for action in draws:
            if action.numIndices < least:
                continue
            controller.SetFrameEvent(action.eventId, True)
            state = controller.GetPipelineState()
            bound = []
            try:
                for entry in state.GetReadOnlyResources(rd.ShaderStage.Pixel):
                    descriptor = getattr(entry, "descriptor", entry)
                    resource = getattr(descriptor, "resource", None)
                    if resource is not None and resource != rd.ResourceId.Null():
                        bound.append(texture_note(textures, resource))
            except Exception:
                pass
            say("  event %-6d indices %-7d %s" % (
                action.eventId, action.numIndices,
                "; ".join(bound) if bound else "(no textures)"))

    elif mode == "pixelxy":
        # The same walk as "pixel", but the point is given in the target's own
        # pixels. Guest colour targets hold bands of the frame rather than the
        # picture, so a fraction of the frame means nothing there; coordinates
        # taken from a dumped target do.
        x = int(os.environ["XE_X"])
        y = int(os.environ["XE_Y"])
        say("probing %d,%d of each draw's target" % (x, y))
        previous = None
        for action in draws:
            controller.SetFrameEvent(action.eventId, True)
            targets = controller.GetPipelineState().GetOutputTargets()
            if not targets or targets[0].resource == rd.ResourceId.Null():
                continue
            resource = targets[0].resource
            texture = textures.get(resource)
            if texture is None or x >= texture.width or y >= texture.height:
                continue
            try:
                value = controller.PickPixel(resource, x, y, rd.Subresource(0, 0, 0),
                                             rd.CompType.Typeless)
                colour = tuple(round(component, 4) for component in value.floatValue[:4])
            except Exception:
                continue
            if colour != previous:
                say("  event %-6d %-40s -> %s  (%d indices)"
                    % (action.eventId, texture_note(textures, resource), colour,
                       action.numIndices))
                previous = colour

    elif mode == "pixel":
        fraction_x = float(os.environ["XE_X"])
        fraction_y = float(os.environ["XE_Y"])
        say("probing the point at %.3f, %.3f of the frame" % (fraction_x, fraction_y))
        previous = None
        for action in draws:
            controller.SetFrameEvent(action.eventId, True)
            targets = controller.GetPipelineState().GetOutputTargets()
            if not targets or targets[0].resource == rd.ResourceId.Null():
                continue
            resource = targets[0].resource
            texture = textures.get(resource)
            if texture is None:
                continue
            x, y = sample_point(texture, fraction_x, fraction_y)
            try:
                value = controller.PickPixel(resource, x, y, rd.Subresource(0, 0, 0),
                                             rd.CompType.Typeless)
                colour = tuple(round(component, 4) for component in value.floatValue[:4])
            except Exception:
                continue
            if colour != previous:
                say("  event %-5d %-38s at %4d,%-4d -> %s  (%d indices)"
                    % (action.eventId, texture_note(textures, resource), x, y, colour,
                       action.numIndices))
                previous = colour

    elif mode == "draw":
        for event in os.environ["XE_EVENTS"].split(","):
            describe_draw(controller, textures, int(event),
                          os.environ.get("XE_SHADERS") == "1")

    elif mode == "textures":
        directory = os.environ["XE_TEXTURE_DIR"]
        os.makedirs(directory, exist_ok=True)
        # Contents are whatever the replay is standing at, so move to the end of
        # the frame first: otherwise every render target comes out as it was
        # before anything was drawn into it.
        # Render targets are recycled as the frame goes on, so by the end most
        # of them are blank. XE_AT_EVENT dumps them as they stood at a chosen
        # draw instead, which is the only way to see what a pass actually wrote.
        at = os.environ.get("XE_AT_EVENT")
        if at:
            controller.SetFrameEvent(int(at), True)
        elif actions:
            controller.SetFrameEvent(actions[-1].eventId, True)
        written = 0
        for texture in controller.GetTextures():
            save = rd.TextureSave()
            save.resourceId = texture.resourceId
            save.destType = rd.FileType.PNG
            # The guest's colour targets are multisampled, and a multisampled
            # texture saved without naming a sample comes out blank.
            if getattr(texture, "msSamp", 1) > 1:
                save.sample.sampleIndex = 0
                save.sample.mapToArray = False
            name = "%s_%dx%d_%s.png" % (texture.resourceId, texture.width, texture.height,
                                        texture.format.Name().replace(" ", ""))
            try:
                controller.SaveTexture(save, os.path.join(directory, name))
                written += 1
            except Exception:
                pass
        say("wrote %d textures to %s" % (written, directory))

    controller.Shutdown()
    capture.Shutdown()


try:
    main()
except Exception:
    say("FAILED")
    say(traceback.format_exc())
OUT.flush()
OUT.close()
sys.exit(0)
