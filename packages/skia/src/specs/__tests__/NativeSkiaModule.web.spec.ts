/**
 * @jest-environment jsdom
 */
import type {
  ISkiaViewApiWeb,
  SkiaWebViewHandle,
} from "../NativeSkiaModule.web";
import "../NativeSkiaModule.web";

const api = () => global.SkiaViewApi as ISkiaViewApiWeb;

const handle = {} as SkiaWebViewHandle;

let frameCallbacks: FrameRequestCallback[] = [];

beforeEach(() => {
  frameCallbacks = [];
  jest
    .spyOn(window, "requestAnimationFrame")
    .mockImplementation((callback: FrameRequestCallback) => {
      frameCallbacks.push(callback);
      return frameCallbacks.length;
    });
});

afterEach(() => {
  jest.restoreAllMocks();
  api().unregisterView("1");
});

const runFrame = () => {
  const callbacks = frameCallbacks;
  frameCallbacks = [];
  callbacks.forEach((callback) => callback(0));
};

describe("SkiaViewApi.whenFrameReady on the web", () => {
  it("resolves on the next animation frame of a registered view", async () => {
    api().registerView("1", handle);
    let resolved = false;
    const ready = api()
      .whenFrameReady(1)
      .then(() => {
        resolved = true;
      });
    await Promise.resolve();
    expect(resolved).toBe(false);
    expect(frameCallbacks).toHaveLength(1);
    runFrame();
    await ready;
    expect(resolved).toBe(true);
  });

  it("rejects for a view that is not registered", async () => {
    await expect(api().whenFrameReady(2)).rejects.toThrow(
      "Cannot wait for a frame: view with nativeID 2 is not registered (it may have unmounted)"
    );
    expect(frameCallbacks).toHaveLength(0);
  });

  it("rejects for a view that unregistered", async () => {
    api().registerView("1", handle);
    api().unregisterView("1");
    await expect(api().whenFrameReady(1)).rejects.toThrow(
      "view with nativeID 1 is not registered"
    );
  });
});
