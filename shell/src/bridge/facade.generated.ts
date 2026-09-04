/**
 * GENERATED FILE — DO NOT EDIT. Produced by tools/contract_gen.py.
 *
 * Typed proxies over the facade. Each method encodes its arguments with the generated codec,
 * calls across the boundary by *name*, and decodes a Result — so a client never sees a method id
 * and cannot be broken by one shifting.
 */
import type * as C from '../../../contracts/ts/contracts';
import { Reader, Writer } from './wire';
import type { FacadeCall } from './facade';
import { decodeStatus, malformedResponse } from './facade';
import * as codec from './codec.generated';

export function createCaptureSessionManagerProxy(call: FacadeCall) {
  return {
    async begin(project: C.ProjectId, spec: C.CapturePlanSpec) {
      const args = new Writer();
      args.f64(project);
      codec.encodeCapturePlanSpec(args, spec);
      const raw = await call('CaptureSessionManager.begin', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = input.f64() as C.SessionId;
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.begin returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async resume(project: C.ProjectId) {
      const args = new Writer();
      args.f64(project);
      const raw = await call('CaptureSessionManager.resume', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = input.f64() as C.SessionId;
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.resume returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async getPlan() {
      const args = new Writer();
      const raw = await call('CaptureSessionManager.getPlan', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.decodeCapturePlan(input);
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.getPlan returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async onMotion(samples: C.ImuSample[]) {
      const args = new Writer();
      args.count(samples.length);
  for (const item of samples) { codec.encodeImuSample(args, item); }
      const raw = await call('CaptureSessionManager.onMotion', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.decodeCaptureGuidance(input);
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.onMotion returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async armBurst(node: C.NodeId, burst: C.BurstSpec) {
      const args = new Writer();
      args.f64(node);
      codec.encodeBurstSpec(args, burst);
      const raw = await call('CaptureSessionManager.armBurst', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
    async offerFrame(node: C.NodeId, frame: C.FrameRef, pose: C.PoseSample) {
      const args = new Writer();
      args.f64(node);
      codec.encodeFrameRef(args, frame);
      codec.encodePoseSample(args, pose);
      const raw = await call('CaptureSessionManager.offerFrame', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.FrameVerdictValues[input.i32()];
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.offerFrame returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async coverage() {
      const args = new Writer();
      const raw = await call('CaptureSessionManager.coverage', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.decodeCoverageState(input);
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.coverage returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async candidates(node: C.NodeId) {
      const args = new Writer();
      args.f64(node);
      const raw = await call('CaptureSessionManager.candidates', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = Array.from({ length: input.count() }, () => codec.decodeCandidate(input));
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: CaptureSessionManager.candidates returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async requestRetake(node: C.NodeId, replace: boolean) {
      const args = new Writer();
      args.f64(node);
      args.bool(replace);
      const raw = await call('CaptureSessionManager.requestRetake', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
    async end() {
      const args = new Writer();
      const raw = await call('CaptureSessionManager.end', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
  };
}

export function createPanoramaBuildManagerProxy(call: FacadeCall) {
  return {
    async start(session: C.SessionId, spec: C.BuildSpec) {
      const args = new Writer();
      args.f64(session);
      codec.encodeBuildSpec(args, spec);
      const raw = await call('PanoramaBuildManager.start', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = input.f64() as C.BuildId;
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: PanoramaBuildManager.start returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async poll(build: C.BuildId) {
      const args = new Writer();
      args.f64(build);
      const raw = await call('PanoramaBuildManager.poll', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.decodeBuildProgress(input);
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: PanoramaBuildManager.poll returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async panorama(build: C.BuildId) {
      const args = new Writer();
      args.f64(build);
      const raw = await call('PanoramaBuildManager.panorama', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.decodePanoramaRef(input);
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: PanoramaBuildManager.panorama returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async ghosts(build: C.BuildId) {
      const args = new Writer();
      args.f64(build);
      const raw = await call('PanoramaBuildManager.ghosts', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = codec.decodeGhostReport(input);
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: PanoramaBuildManager.ghosts returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async invalidate(build: C.BuildId, dirty: C.NodeId[]) {
      const args = new Writer();
      args.f64(build);
      args.count(dirty.length);
  for (const item of dirty) { args.f64(item); }
      const raw = await call('PanoramaBuildManager.invalidate', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
    async cancel(build: C.BuildId) {
      const args = new Writer();
      args.f64(build);
      const raw = await call('PanoramaBuildManager.cancel', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
  };
}

export function createProjectManagerProxy(call: FacadeCall) {
  return {
    async list() {
      const args = new Writer();
      const raw = await call('ProjectManager.list', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = Array.from({ length: input.count() }, () => codec.decodeProjectSummary(input));
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: ProjectManager.list returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async create(title: string) {
      const args = new Writer();
      args.string(title);
      const raw = await call('ProjectManager.create', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      if (status.code !== 'Ok') return { ok: false, status } as const;
      const value = input.f64() as C.ProjectId;
      if (!input.ok) return { ok: false, status: malformedResponse(
        'malformed response: ProjectManager.create returned a value that did not decode') } as const;
      return { ok: true, value } as const;
    },
    async delete(project: C.ProjectId) {
      const args = new Writer();
      args.f64(project);
      const raw = await call('ProjectManager.delete', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
    async setSelection(project: C.ProjectId, node: C.NodeId, candidate: C.CandidateId) {
      const args = new Writer();
      args.f64(project);
      args.f64(node);
      args.f64(candidate);
      const raw = await call('ProjectManager.setSelection', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
    async export(project: C.ProjectId, build: C.BuildId, spec: C.ExportSpec) {
      const args = new Writer();
      args.f64(project);
      args.f64(build);
      codec.encodeExportSpec(args, spec);
      const raw = await call('ProjectManager.export', args.finish());
      const input = new Reader(raw);
      const status = decodeStatus(input);
      return status.code === 'Ok'
        ? ({ ok: true, value: undefined } as const)
        : ({ ok: false, status } as const);
    },
  };
}
