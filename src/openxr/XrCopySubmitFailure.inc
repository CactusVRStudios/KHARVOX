    if(submitResult!=VK_SUCCESS){
        s.fsr1.discardRecordedFrame();
        if(freshAerHands)s.freshHandsWorldValid=false;
        if(nativeFrameValid)kharvox::native::fail("owner native copy submission failed; restart required");
        s.handRenderer.finishSceneIntegratedFrame();
        releaseAcquiredEyeImages();
        if(s.hudImageState.owned())releaseImageChecked(s.hudQuad.handle,s.hudImageState);
        log("copy submit failed "+std::to_string(submitResult));endEmptyFrame("copy-submit-failed");return;
    }
