/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2016 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Application
    potentialFoam

Description
    Potential flow solver with immersed boundary support.

\*---------------------------------------------------------------------------*/

#include "fvCFD.H"
#include "fvIOoptionList.H"
#include "simpleControl.H"
#include "immersedBoundaryFvPatch.H"
#include "immersedBoundaryAdjustPhi.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

int main(int argc, char *argv[])
{
    argList::addBoolOption
    (
        "writep",
        "Calculate and write the pressure field"
    );

    argList::addBoolOption
    (
        "withFunctionObjects",
        "execute functionObjects"
    );

    #include "setRootCase.H"
    #include "createTime.H"
    #include "createMesh.H"
    #include "readControls.H"
    #include "createFields.H"
    #include "createFvOptions.H"

    // * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

    Info<< nl << "Calculating potential flow" << endl;

    // Since solver contains no time loop it would never execute
    // function objects so do it ourselves
    runTime.functionObjects().start();

    fvOptions.makeRelative(phi);

    adjustPhi(phi, U, p);

    // Non-orthogonal velocity potential corrector loop
    for (int nonOrth=0; nonOrth<=nNonOrthCorr; nonOrth++)
    {
        phi = faceIbMask*(linearInterpolate(U) & mesh.Sf());

        // Adjust immersed boundary fluxes
        immersedBoundaryAdjustPhi(phi, U);

        // Adjust fluxes
        adjustPhi(phi, U, p);

        p.storePrevIter();

        fvScalarMatrix pEqn
        (
            fvm::laplacian
            (
                dimensionedScalar
                (
                    "1",
                    dimTime/p.dimensions()*dimensionSet(0, 2, -2, 0, 0),
                    1
                ),
                p
            )
         ==
            fvc::div(phi)
        );

        pEqn.setReference(pRefCell, pRefValue);

        pEqn.boundaryManipulate(p.boundaryField());

        pEqn.solve();

        if (nonOrth == nNonOrthCorr)
        {
            phi -= pEqn.flux();
        }
    }

    fvOptions.makeAbsolute(phi);


    Info<< "p min " << gMin(p.internalField())
        << " max " << gMax(p.internalField())
        << " masked min "
        << gMin(cellIbMask.internalField()*p.internalField())
        << " max "
        << gMax(cellIbMask.internalField()*p.internalField())
        << endl;

    Info<< "Continuity error = "
        << mag(fvc::div(faceIbMask*phi))().weightedAverage(mesh.V()).value()
        << endl;

    Info<< "Contour continuity error = " << mag(sum(phi.boundaryField()))
        << endl;

    U = fvc::reconstruct(phi);
    U.correctBoundaryConditions();

    Info<< "Interpolated velocity error = "
        << (
                sqrt
                (
                    sum
                    (
                        sqr
                        (
                            faceIbMask*
                            (
                                fvc::interpolate(U) & mesh.Sf()
                            )
                            - phi
                        )
                    )
                )/sum(mesh.magSf())
            ).value()
        << endl;

    // Calculate velocity magnitude
    {
        volScalarField magU = cellIbMask*mag(U);

        Info << "IB-masked mag(U): max: " << gMax(magU.internalField())
            << " min: " << gMin(magU.internalField()) << endl;
    }

    // Write U and phi
    U.write();
    phi.write();
//     p.write();

    // Calculate the pressure field
    if (args.optionFound("writep"))
    {
        Info<< nl << "Calculating approximate pressure field" << endl;

        label refPatch = -1;
        scalar maxMagU = 0;

        // Go through all velocity patches and find the one that fixes
        // velocity to the largest value

        forAll (U.boundaryField(), patchI)
        {
            const fvPatchVectorField& Upatch = U.boundaryField()[patchI];

            if (Upatch.fixesValue())
            {
                // Calculate mean velocity
                scalar u = sum(mag(Upatch));
                label patchSize = Upatch.size();

                reduce(u, sumOp<scalar>());
                reduce(patchSize, sumOp<label>());

                if (patchSize > 0)
                {
                    scalar curMag = u/patchSize;

                    if (curMag > maxMagU)
                    {
                        refPatch = patchI;

                        maxMagU = curMag;
                    }
                }
            }
        }

        if (refPatch > -1)
        {
            // Calculate reference pressure
            const fvPatchVectorField& Upatch = U.boundaryField()[refPatch];
            const fvPatchScalarField& pPatch = p.boundaryField()[refPatch];

            scalar patchE = sum(mag(pPatch + 0.5*magSqr(Upatch)));
            label patchSize = Upatch.size();

            reduce(patchE, sumOp<scalar>());
            reduce(patchSize, sumOp<label>());

            scalar e = patchE/patchSize;


            Info<< "Using reference patch " << refPatch
                << " with mag(U) = " << maxMagU
                << " p + 0.5*U^2 = " << e << endl;

            p.internalField() = e - 0.5*magSqr(U.internalField());
            p.correctBoundaryConditions();
        }
        else
        {
            Info<< "No reference patch found.  Writing potential function"
                << endl;
        }

        p.write();
    }

    runTime.functionObjects().end();

    Info<< "ExecutionTime = " << runTime.elapsedCpuTime() << " s"
        << "  ClockTime = " << runTime.elapsedClockTime() << " s"
        << nl << endl;

    Info<< "End\n" << endl;

    return 0;
}


// ************************************************************************* //
